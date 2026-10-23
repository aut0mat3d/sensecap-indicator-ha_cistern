/**
 * @file sdcard.cpp
 * @brief Full Non-Blocking MicroSD Driver, MBR Partitioning, VBR Patching & Verification Engine.
 */

#include "sdcard.hpp"
#include "littlefs_buffer.hpp"
#include "littlefs_driver.hpp"
#include <ArduinoJson.h>

/* Extern global PacketSerial instance defined in main.cpp */
extern PacketSerial packetSerial;

static bool g_config_reload_pending = false;

/* ── Private MBR Data Structures (Packed) ───────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  boot_indicator; /* 0x00 = Non-bootable, 0x80 = Bootable */
    uint8_t  start_head;
    uint8_t  start_sector_cyl;
    uint8_t  start_cyl;
    uint8_t  partition_type; /* 0x0C = FAT32 LBA, 0x83 = Linux / LittleFS */
    uint8_t  end_head;
    uint8_t  end_sector_cyl;
    uint8_t  end_cyl;
    uint32_t start_lba;      /* Start sector in LBA */
    uint32_t sector_count;   /* Number of sectors in partition */
} mbr_partition_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  boot_code[446];
    mbr_partition_entry_t partitions[4];
    uint16_t signature;      /* 0xAA55 */
} mbr_sector_t;

/* ── Private BlockDevice Wrapper Class ─────────────────────────────────── */

class PartitionBlockDevice : public FsBlockDevice {
private:
    FsBlockDevice* m_dev;
    uint32_t m_startLba;
    uint32_t m_sectorCount;

public:
    PartitionBlockDevice(FsBlockDevice* dev, uint32_t startLba, uint32_t sectorCount)
        : m_dev(dev), m_startLba(startLba), m_sectorCount(sectorCount) {}

    bool isBusy() override { return m_dev ? m_dev->isBusy() : false; }
    uint32_t sectorCount() override { return m_sectorCount; }

    bool readSector(uint32_t sector, uint8_t* dst) override {
        if (sector >= m_sectorCount) return false;
        return m_dev->readSector(m_startLba + sector, dst);
    }

    bool readSectors(uint32_t sector, uint8_t* dst, size_t ns) override {
        if (sector + ns > m_sectorCount) return false;
        return m_dev->readSectors(m_startLba + sector, dst, ns);
    }

    bool writeSector(uint32_t sector, const uint8_t* src) override {
        if (sector >= m_sectorCount) return false;
        return m_dev->writeSector(m_startLba + sector, src);
    }

    bool writeSectors(uint32_t sector, const uint8_t* src, size_t ns) override {
        if (sector + ns > m_sectorCount) return false;
        return m_dev->writeSectors(m_startLba + sector, src, ns);
    }

    bool syncDevice() override { return m_dev ? m_dev->syncDevice() : false; }
};

/* ── Private Global Driver Variables ───────────────────────────────────── */

static SdFat32 sd;
static Adafruit_USBD_MSC usb_msc;

static bool g_sd_initialized = false;
static bool g_usb_msc_active = false;
static char g_expected_label[12] = "SCAP_UNK";

/* Card Detect ISR Variable */
static volatile bool g_sd_card_event_triggered = false;
/* Card Detect Hardware Debounce Variables */
static volatile bool     g_sd_card_edge_detected = false;
static volatile uint32_t g_sd_last_edge_ms = 0;
static int8_t            g_last_pushed_state = -99;
static uint8_t           g_last_pushed_present = 255;

/* Non-blocking Format State Machine Variables */
typedef enum {
    FMT_IDLE = 0,
    FMT_START,
    FMT_WRITE_INITIAL_MBR,
    FMT_RUN_FAT_FORMATTER,
    FMT_WAIT_NAND_BUSY,
    FMT_PATCH_MBR_AND_VBR,
    FMT_MOUNT_FAT32,
    FMT_CREATE_HANDSHAKE_FILE,
    FMT_VERIFY, /* Strict Post-Format Verification State */
    FMT_SUCCESS,
    FMT_ERROR
} format_state_t;

static format_state_t g_fmt_state = FMT_IDLE;
static uint32_t g_fmt_step_start_ms = 0;
static uint32_t g_fmt_total_start_ms = 0;
static char     g_fmt_target_label[12] = {0};
static uint8_t  g_fmt_type = 1;

#define FMT_TOTAL_TIMEOUT_MS  12000 /* 12 seconds global safety timeout */
#define FMT_BUSY_TIMEOUT_MS   3000  /* 3 seconds max wait for flash write sync */

/* ── TinyUSB MSC Callbacks ─────────────────────────────────────────────── */

static int32_t msc_read_cb(uint32_t lba, void* buffer, uint32_t bufsize) {
    return sd.card()->readSectors(lba, (uint8_t*)buffer, bufsize / 512) ? bufsize : -1;
}

static int32_t msc_write_cb(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
    return sd.card()->writeSectors(lba, buffer, bufsize / 512) ? bufsize : -1;
}

static void msc_flush_cb(void) {
    if (sd.card()) sd.card()->syncDevice();
}

/* ── Card Detect Hardware ISR ──────────────────────────────────────────── */

static void gpio7_sd_detect_isr(void) {
    g_sd_card_edge_detected = true;
    g_sd_last_edge_ms = millis();
}
/* ── MBR & VBR Low-Level Patch Helpers ─────────────────────────────────── */

static bool write_custom_mbr(uint32_t p1_start, uint32_t p1_size) {
    if (!sd.card()) return false;

    mbr_sector_t mbr;
    memset(&mbr, 0, sizeof(mbr_sector_t));

    /* Partition 1: FAT32 LBA (Type 0x0C) starting at p1_start */
    mbr.partitions[0].boot_indicator = 0x00;
    mbr.partitions[0].partition_type = 0x0C;
    mbr.partitions[0].start_lba     = p1_start;
    mbr.partitions[0].sector_count  = p1_size;

    /* Partition 2: 1 GB Linux/LittleFS (Type 0x83) starting at LBA 4456448 */
    mbr.partitions[1].boot_indicator = 0x00;
    mbr.partitions[1].partition_type = 0x83;
    mbr.partitions[1].start_lba     = 4456448;
    mbr.partitions[1].sector_count  = 2097152; /* 1024 MB */

    mbr.signature = 0xAA55;

    if (!sd.card()->writeSectors(0, (const uint8_t*)&mbr, 1)) return false;
    sd.card()->syncDevice();
    return true;
}

static bool apply_fat32_volume_label(uint32_t p1_lba, const char* target_label) {
    if (!sd.card()) return false;

    uint8_t sector_buf[512];
    if (!sd.card()->readSectors(p1_lba, sector_buf, 1)) return false;

    uint8_t formatted_label[11];
    memset(formatted_label, ' ', 11);
    size_t len = strlen(target_label);
    if (len > 11) len = 11;
    memcpy(formatted_label, target_label, len);

    /* Patch BPB_HiddSec at offset 0x1C */
    memcpy(&sector_buf[0x1C], &p1_lba, sizeof(uint32_t));

    /* Patch VBR Volume Label at offset 0x47 */
    memcpy(&sector_buf[0x47], formatted_label, 11);
    if (!sd.card()->writeSectors(p1_lba, sector_buf, 1)) return false;

    /* Calculate Root Directory LBA */
    uint16_t rsvd_sec_cnt = 0;
    uint8_t  num_fats     = 0;
    uint32_t fat_sz32     = 0;
    uint32_t root_clus    = 0;
    uint8_t  sec_per_clus = sector_buf[0x0D];

    memcpy(&rsvd_sec_cnt, &sector_buf[0x0E], sizeof(uint16_t));
    num_fats = sector_buf[0x10];
    memcpy(&fat_sz32, &sector_buf[0x24], sizeof(uint32_t));
    memcpy(&root_clus, &sector_buf[0x2C], sizeof(uint32_t));

    uint32_t fat_start_lba  = p1_lba + rsvd_sec_cnt;
    uint32_t data_start_lba = fat_start_lba + (num_fats * fat_sz32);
    uint32_t root_dir_lba   = data_start_lba + ((root_clus - 2) * sec_per_clus);

    /* Patch Root Directory Entry 0 with Attribute 0x08 */
    if (sd.card()->readSectors(root_dir_lba, sector_buf, 1)) {
        memset(&sector_buf[0], 0, 32);
        memcpy(&sector_buf[0], formatted_label, 11);
        sector_buf[11] = 0x08;
        sd.card()->writeSectors(root_dir_lba, sector_buf, 1);
    }

    sd.card()->syncDevice();
    return true;
}

/* ── TinyUSB MSC Start/Stop (Eject) Callback ──────────────────────────── */

static bool msc_start_stop_cb(uint8_t power_condition, bool start, bool load_eject) {
    (void) power_condition;
    
    /* PC Host issued an Eject / Unmount command ("Sicher entfernen") */
    if (load_eject && !start) {
        g_usb_msc_active = false;
        usb_msc.setUnitReady(false);
        Serial.println("TinyUSB MSC Mode: Ejected by PC host!\r\n");

        /* Re-initialize SdFat to read freshly flushed sectors from PC */
        SdSpiConfig spi_config(SD_PIN_CS, DEDICATED_SPI, SD_SCK_MHZ(24), (SpiPort_t*)&SPI1);
        g_sd_initialized = sd.begin(spi_config);

        /* Push updated state immediately to ESP32 */
        g_sd_card_event_triggered = true;
    }
    return true;
}

/* ── Public API Implementation ─────────────────────────────────────────── */

bool sd_card_is_msc_active(void) {
    return g_usb_msc_active;
}

void sd_card_init(void) {
    static bool hardware_initialized = false;

    /* 1. Register hardware pins & TinyUSB MSC callbacks ONLY ONCE at boot */
    if (!hardware_initialized) {
        SPI1.setSCK(SD_PIN_SCK);
        SPI1.setTX(SD_PIN_MOSI);
        SPI1.setRX(SD_PIN_MISO);
        SPI1.setCS(SD_PIN_CS);

        pinMode(SD_PIN_DET, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(SD_PIN_DET), gpio7_sd_detect_isr, CHANGE);

        /* Register TinyUSB MSC callbacks & descriptor at boot */
        usb_msc.setID("Seeed", "SenseCAP SD", "1.0");
        usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
        usb_msc.setStartStopCallback(msc_start_stop_cb);
        usb_msc.setUnitReady(false);
        usb_msc.begin();

        hardware_initialized = true;
    }

    /* 2. Check hardware card detect pin (GPIO 7) */
    if (digitalRead(SD_PIN_DET) != LOW) {
        g_sd_initialized = false;
        return;
    }

    /* 3. Initialize SdFat driver on SPI1 bus */
    SdSpiConfig spi_config(SD_PIN_CS, DEDICATED_SPI, SD_SCK_MHZ(24), (SpiPort_t*)&SPI1);
    g_sd_initialized = sd.begin(spi_config);

    if (g_sd_initialized && sd.card()) {
        usb_msc.setCapacity(sd.card()->sectorCount(), 512);
    }
}

bool sd_card_has_event_pending(void) {
    return g_sd_card_event_triggered;
}

void sd_card_clear_event_pending(void) {
    g_sd_card_event_triggered = false;
}

bool sd_card_needs_config_reload(void) {
    return g_config_reload_pending;
}

void sd_card_clear_config_reload(void) {
    g_config_reload_pending = false;
}

/* In rp2040/src/sdcard.cpp */
void sd_card_trigger_status_push(void) {
    uint8_t present = 0, usb_mounted = 0;
    int8_t state = SD_STATE_UNCHECKED;
    uint32_t capacity_mb = 0;
    char volume_label[12] = {0};

    sd_card_get_info(present, usb_mounted, state, capacity_mb, volume_label);

    uint8_t res_buf[24] = {0};
    res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_SD_INFO);
    res_buf[1] = present;
    res_buf[2] = usb_mounted;
    res_buf[3] = static_cast<uint8_t>(state);
    memcpy(&res_buf[4], &capacity_mb, sizeof(capacity_mb));
    memcpy(&res_buf[8], volume_label, 11);

    packetSerial.send(res_buf, 19);

    /* Push LoRa config automatically to ESP32 on status query if SD card is valid */
    if (state == SD_STATE_VALID_FAT32_LFS || state == SD_STATE_VALID_FAT32) {
        g_config_reload_pending = true;
    }
}

SdFat32& sd_card_get_fs(void) {
    return sd;
}

void sd_card_set_expected_label(const char* label) {
    if (label && strlen(label) > 0) {
        strncpy(g_expected_label, label, sizeof(g_expected_label) - 1);
        g_expected_label[11] = '\0';
    }
}

void sd_card_msc_toggle(bool enable) {
    if (enable && g_sd_initialized && sd.card()) {
        usb_msc.setCapacity(sd.card()->sectorCount(), 512);
        usb_msc.setUnitReady(true); /* Signal medium insertion to PC */
        g_usb_msc_active = true;
        Serial.println("TinyUSB MSC Mode: ENABLED\r\n");
    } else {
        usb_msc.setUnitReady(false); /* Signal medium removal to PC */
        g_usb_msc_active = false;
        Serial.println("TinyUSB MSC Mode: DISABLED\r\n");

        /* Re-initialize SdFat to re-read VBR/FAT modified by PC! */
        SdSpiConfig spi_config(SD_PIN_CS, DEDICATED_SPI, SD_SCK_MHZ(24), (SpiPort_t*)&SPI1);
        g_sd_initialized = sd.begin(spi_config);
    }

    /* Push updated MSC state immediately to ESP32 */
    g_sd_card_event_triggered = true;

    /* Flush pending sensor records buffered in RAM while PC held exclusive SD lock */
    if (!enable && littlefs_driver_is_mounted()) {
        littlefs_buffer_flush_all();
    }
}

void sd_card_get_info(uint8_t &is_present, uint8_t &is_usb_mounted, int8_t &state, uint32_t &capacity_mb, char* volume_label) {
    static uint32_t last_retry_ms = 0;

    /* 1. Hardware Pin Check ALWAYS comes first (GPIO 7) */
    if (digitalRead(SD_PIN_DET) != LOW) {
        /* Safety: Unmount LittleFS immediately so write routines know SD is gone */
        if (g_sd_initialized || littlefs_driver_is_mounted()) {
            littlefs_driver_unmount();
        }
        g_sd_initialized = false;
        is_present = 0;
        is_usb_mounted = 0;
        state = SD_STATE_UNCHECKED; /* -1 */
        capacity_mb = 0;
        memset(volume_label, 0, 12);
        return;
    }

    /* 2. STRICT EARLY MSC GUARD: Never touch SPI while PC has control */
    if (g_usb_msc_active) {
        is_present = 1;
        is_usb_mounted = 1;
        state = SD_STATE_VALID_FAT32_LFS;
        capacity_mb = 0;
        strncpy(volume_label, g_expected_label, 11);
        return;
    }

    /* 3. STRICT FORMAT LOCK: Block external polling during formatting steps */
    if (g_fmt_state != FMT_IDLE && g_fmt_state != FMT_VERIFY) {
        is_present = 1;
        is_usb_mounted = 0;
        state = SD_STATE_UNCHECKED;
        capacity_mb = 0;
        memset(volume_label, 0, 12);
        return;
    }

    /* Cooldown Guard: Retry SD initialization at most once every 1000ms if not initialized */
    if (!g_sd_initialized) {
        if (millis() - last_retry_ms > 1000) {
            last_retry_ms = millis();
            sd_card_init();
        }
    }

    if (!g_sd_initialized) {
        is_present = 1;
        is_usb_mounted = 0;
        state = SD_STATE_MISMATCH; /* 0 */
        capacity_mb = 0;
        memset(volume_label, 0, 12);
        return;
    }

    is_present = 1;
    is_usb_mounted = 0;

    if (sd.card()) {
        uint64_t total_sectors = sd.card()->sectorCount();
        capacity_mb = static_cast<uint32_t>((total_sectors * 512ULL) / (1024ULL * 1024ULL));
    } else {
        capacity_mb = 0;
    }

    /* Read VBR Label directly via P1 Start LBA from MBR */
    char native_vbr_label[12] = {0};
    bool vbr_label_valid = false;

    if (sd.card()) {
        static uint8_t sector_buf[512];
        uint32_t p1_lba = 131072; /* Fallback LBA */

        if (sd.card()->readSectors(0, sector_buf, 1)) {
            if (sector_buf[0x1FE] == 0x55 && sector_buf[0x1FF] == 0xAA) {
                memcpy(&p1_lba, &sector_buf[0x01C6], sizeof(uint32_t));
            }
        }

        if (sd.card()->readSectors(p1_lba, sector_buf, 1)) {
            memcpy(native_vbr_label, &sector_buf[0x47], 11);
            for (int i = 10; i >= 0; i--) {
                if (native_vbr_label[i] == ' ' || native_vbr_label[i] == '\0') native_vbr_label[i] = '\0';
                else break;
            }
            if (strlen(native_vbr_label) > 0 && strncmp(native_vbr_label, "NO NAME", 7) != 0) {
                vbr_label_valid = true;
            }
        }
    }

    if (!vbr_label_valid) strncpy(native_vbr_label, "UNLABELED", 11);
    strncpy(volume_label, native_vbr_label, 11);

    /* STRICT DEVICE OWNERSHIP GUARD:
       Verify volume label matches expected ESP32 MAC label (e.g. "SCAP_75A140").
       If ESP32 hasn't synced its MAC yet ("SCAP_UNK"), block config access until MAC is pushed. */
    bool label_matches = (strlen(g_expected_label) > 0 && 
                          strncmp(g_expected_label, "SCAP_UNK", 8) != 0 && 
                          strncmp(native_vbr_label, g_expected_label, 11) == 0);

    /* Self-healing missing /.volume_label file */
    if (label_matches && !sd.exists("/.volume_label")) {
        FatFile auto_repair_file;
        if (auto_repair_file.open("/.volume_label", O_WRITE | O_CREAT | O_TRUNC)) {
            auto_repair_file.write(g_expected_label, strlen(g_expected_label));
            auto_repair_file.close();
            Serial.println("SD Health Check: Self-healed missing /.volume_label file.\r\n");
        }
    }

    /* MBR Validation for Partition 2 */
    bool p2_mbr_valid = false;
    if (sd.card()) {
        static uint8_t mbr_buf[512];
        if (sd.card()->readSectors(0, mbr_buf, 1)) {
            if (mbr_buf[0x1FE] == 0x55 && mbr_buf[0x1FF] == 0xAA) {
                uint8_t  p2_type = mbr_buf[0x01D2];
                uint32_t p2_start = 0;
                uint32_t p2_sectors = 0;
                memcpy(&p2_start, &mbr_buf[0x01D6], sizeof(uint32_t));
                memcpy(&p2_sectors, &mbr_buf[0x01DA], sizeof(uint32_t));

                if (p2_type == 0x83 && p2_start == 4456448 && p2_sectors == 2097152) {
                    p2_mbr_valid = true;
                }
            }
        }
    }

    /* Mount & verify LittleFS on Partition 2 */
    bool p2_lfs_valid = false;
    if (label_matches && p2_mbr_valid) {
        p2_lfs_valid = littlefs_driver_is_mounted() || littlefs_driver_mount();
    }

    if (label_matches && p2_mbr_valid && p2_lfs_valid) {
        state = SD_STATE_VALID_FAT32_LFS; /* 3: Beide Partitionen voll verifiziert & bereit! */
    } else if (label_matches && p2_mbr_valid) {
        state = SD_STATE_VALID_FAT32;     /* 2: FAT32 passt, LittleFS noch unformatiert */
    } else {
        state = SD_STATE_MISMATCH;        /* 0: Fremde Karte oder ungültiges Label */
    }
}

bool sd_card_format_start(uint8_t format_type, const char* new_label) {
    if (g_fmt_state != FMT_IDLE) return false;

    g_fmt_type = format_type;
    strncpy(g_fmt_target_label, new_label, sizeof(g_fmt_target_label) - 1);
    g_fmt_target_label[11] = '\0';

    g_fmt_total_start_ms = millis();
    g_fmt_step_start_ms = millis();
    g_fmt_state = FMT_START;
    return true;
}

/* ── Non-Blocking State Machine Executed in loop1() ────────────────────── */

void sd_card_update(PacketSerial &packetSerial) {
    /* 1. Trailing-Edge Debouncing */
    if (g_sd_card_edge_detected) {
        if (millis() - g_sd_last_edge_ms >= 300) {
            g_sd_card_edge_detected = false;

            uint8_t present = 0, usb_mounted = 0;
            int8_t state = SD_STATE_UNCHECKED;
            uint32_t capacity_mb = 0;
            char volume_label[12] = {0};

            sd_card_get_info(present, usb_mounted, state, capacity_mb, volume_label);

            if (state != g_last_pushed_state || present != g_last_pushed_present) {
                g_last_pushed_state = state;
                g_last_pushed_present = present;

                uint8_t res_buf[24] = {0};
                res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_SD_INFO);
                res_buf[1] = present;
                res_buf[2] = usb_mounted;
                res_buf[3] = static_cast<uint8_t>(state);
                memcpy(&res_buf[4], &capacity_mb, sizeof(capacity_mb));
                memcpy(&res_buf[8], volume_label, 11);

                packetSerial.send(res_buf, 19);
                Serial.printf("SD Card Event Pushed! State: %d, Label: %s\r\n", state, volume_label);

                /* Signal main loop to reload LoRa config files from SD */
                g_config_reload_pending = true;
            }
        }
    }

    /* 2. Non-blocking Formatting State Machine */
    if (g_fmt_state == FMT_IDLE) return;

    /* HARDWARE GUARD: Abort format immediately if card was physically pulled */
    if (digitalRead(SD_PIN_DET) != LOW) {
        Serial.println("SD Format Error: Card physically removed during operation!\r\n");
        g_fmt_state = FMT_ERROR;
    }

    /* GLOBAL TIMEOUT GUARD (12 Seconds Total Limit) */
    if (millis() - g_fmt_total_start_ms > FMT_TOTAL_TIMEOUT_MS) {
        Serial.println("SD Format Error: Global timeout reached!\r\n");
        g_fmt_state = FMT_ERROR;
    }

    /* PER-STEP TIMEOUT GUARD (3 Seconds max per step) */
    if (millis() - g_fmt_step_start_ms > FMT_BUSY_TIMEOUT_MS && g_fmt_state != FMT_WAIT_NAND_BUSY) {
        Serial.printf("SD Format Error: Step %d timed out!\r\n", g_fmt_state);
        g_fmt_state = FMT_ERROR;
    }

    switch (g_fmt_state) {
        case FMT_START:
         Serial.printf("SD Format: Starting machine for type=%d, label=%s\r\n", g_fmt_type, g_fmt_target_label);

         if (!g_sd_initialized) {
             sd_card_init();
         }

         if (g_usb_msc_active) {
             usb_msc.setUnitReady(false);
             g_usb_msc_active = false;
         }

         g_fmt_step_start_ms = millis();
         if (g_fmt_type == 0) {
             /* Format LittleFS Partition 2 directly */
             Serial.println("[SD_FORMAT] Formatting LittleFS partition 2...");
             bool ok = littlefs_driver_format();
             g_fmt_state = ok ? FMT_SUCCESS : FMT_ERROR;
         } else {
             g_fmt_state = FMT_WRITE_INITIAL_MBR;
         }
         break;

        case FMT_WRITE_INITIAL_MBR:
            Serial.println("SD Format Step 1: Writing initial MBR at Sector 0...\r\n");
            g_fmt_step_start_ms = millis();
            if (write_custom_mbr(131072, 4325376)) {
                g_fmt_state = FMT_RUN_FAT_FORMATTER;
            } else {
                Serial.println("SD Format Error: MBR write failed!\r\n");
                g_fmt_state = FMT_ERROR;
            }
            break;

        case FMT_RUN_FAT_FORMATTER: {
            Serial.println("SD Format Step 2: Formatting Partition 1 (2.06GB FAT32)...\r\n");
            PartitionBlockDevice part1(sd.card(), 131072, 4325376);
            FatFormatter fatFormatter;
            uint8_t sec_buf[512];
            g_fmt_step_start_ms = millis();
            if (fatFormatter.format(&part1, sec_buf, &Serial)) {
                Serial.println("SD Format Step 2 OK: FatFormatter finished.\r\n");
                g_fmt_step_start_ms = millis();
                g_fmt_state = FMT_WAIT_NAND_BUSY;
            } else {
                Serial.println("SD Format Error: FatFormatter failed!\r\n");
                g_fmt_state = FMT_ERROR;
            }
            break;
        }

        case FMT_WAIT_NAND_BUSY:
            if (sd.card() && !sd.card()->isBusy()) {
                g_fmt_step_start_ms = millis();
                g_fmt_state = FMT_PATCH_MBR_AND_VBR;
            } else if (millis() - g_fmt_step_start_ms > FMT_BUSY_TIMEOUT_MS) {
                if (sd.card()) sd.card()->syncDevice();
                g_fmt_step_start_ms = millis();
                g_fmt_state = FMT_PATCH_MBR_AND_VBR;
            }
            break;

        case FMT_PATCH_MBR_AND_VBR: {
            Serial.println("SD Format Step 3: Patching MBR and VBR Volume Label...\r\n");
            
            uint8_t sec_buf[512];
            uint32_t rel_start = 0;
            if (sd.card()->readSectors(131072, sec_buf, 1)) {
                if (sec_buf[0x1FE] == 0x55 && sec_buf[0x1FF] == 0xAA) {
                    memcpy(&rel_start, &sec_buf[0x1C6], sizeof(uint32_t));
                }
            }

            uint32_t p1_actual_start = 131072 + rel_start;
            uint32_t p1_actual_size  = 4325376 - rel_start;

            g_fmt_step_start_ms = millis();
            if (write_custom_mbr(p1_actual_start, p1_actual_size)) {
                if (apply_fat32_volume_label(p1_actual_start, g_fmt_target_label)) {
                    Serial.println("SD Format Step 3 OK: VBR Label patched.\r\n");
                    g_fmt_step_start_ms = millis();
                    g_fmt_state = FMT_MOUNT_FAT32;
                } else {
                    Serial.println("SD Format Error: Failed to patch VBR volume label!\r\n");
                    g_fmt_state = FMT_ERROR;
                }
            } else {
                Serial.println("SD Format Error: Failed to re-write MBR!\r\n");
                g_fmt_state = FMT_ERROR;
            }
            break;
        }

        case FMT_MOUNT_FAT32: {
            Serial.println("SD Format Step 4: Mounting Partition 1 in SdFat...\r\n");
            SdSpiConfig spi_config(SD_PIN_CS, DEDICATED_SPI, SD_SCK_MHZ(24), (SpiPort_t*)&SPI1);
            g_fmt_step_start_ms = millis();
            if (sd.begin(spi_config)) {
                g_sd_initialized = true;
                g_fmt_step_start_ms = millis();
                g_fmt_state = FMT_CREATE_HANDSHAKE_FILE;
            } else {
                Serial.println("SD Format Error: Partition 1 mount failed!\r\n");
                g_fmt_state = FMT_ERROR;
            }
            break;
        }

        case FMT_CREATE_HANDSHAKE_FILE: {
         Serial.println("SD Format Step 5: Creating /.volume_label file...\r\n");
         FatFile label_file;
         g_fmt_step_start_ms = millis();
         if (label_file.open("/.volume_label", O_WRITE | O_CREAT | O_TRUNC)) {
             label_file.write(g_fmt_target_label, strlen(g_fmt_target_label));
             label_file.close();
             sd.card()->syncDevice();
             sd_card_set_expected_label(g_fmt_target_label);

             /* Format Partition 2 with LittleFS structure */
             Serial.println("SD Format Step 5b: Formatting LittleFS Partition 2...\r\n");
             littlefs_driver_format();

             g_fmt_step_start_ms = millis();
             g_fmt_state = FMT_VERIFY;
         } else {
             Serial.println("SD Format Error: Failed to create /.volume_label file!\r\n");
             g_fmt_state = FMT_ERROR;
         }
         break;
     }

        case FMT_VERIFY: {
            Serial.println("SD Format Step 6: Verifying format integrity...\r\n");
            uint8_t present = 0, usb_mounted = 0;
            int8_t verify_state = SD_STATE_UNCHECKED;
            uint32_t capacity_mb = 0;
            char volume_label[12] = {0};

            g_fmt_step_start_ms = millis();
            sd_card_get_info(present, usb_mounted, verify_state, capacity_mb, volume_label);

            if (verify_state == SD_STATE_VALID_FAT32_LFS) {
                Serial.println("SD Format Verification PASSED: Dual-Partition & Labels fully verified!\r\n");
                g_fmt_state = FMT_SUCCESS;
            } else {
                Serial.printf("SD Format Verification FAILED: Post-format state was %d!\r\n", verify_state);
                g_fmt_state = FMT_ERROR;
            }
            break;
        }

        case FMT_SUCCESS: {
            uint8_t res_buf[2] = { static_cast<uint8_t>(PKT_TYPE_RES_SD_FORMAT), 1 };
            packetSerial.send(res_buf, sizeof(res_buf));
            g_sd_card_event_triggered = true;
            Serial.println("Non-Blocking Dual-Partition Format completed successfully!\r\n");
            g_fmt_state = FMT_IDLE;
            break;
        }

        case FMT_ERROR: {
            g_sd_initialized = false;
            uint8_t res_buf[2] = { static_cast<uint8_t>(PKT_TYPE_RES_SD_FORMAT), 0 };
            packetSerial.send(res_buf, sizeof(res_buf));
            g_sd_card_event_triggered = true;
            Serial.println("Non-Blocking Dual-Partition Format failed.\r\n");
            g_fmt_state = FMT_IDLE;
            break;
        }

        default:
            g_fmt_state = FMT_IDLE;
            break;
    }
}

bool save_time_config_to_sd(SdFat32 &sd, const char* posix_tz, const char* format_str) {
    File32 file = sd.open("/lora_p2p_configs/globals.json", O_READ);
    if (!file) return false;

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) return false;

    /* Update time configuration nodes */
    JsonObject time_cfg = doc["time_config"].createNestedObject();
    time_cfg["posix_timezone"] = posix_tz;
    time_cfg["time_format"] = format_str;

    /* Write back updated JSON to SD card */
    file = sd.open("/lora_p2p_configs/globals.json", O_WRITE | O_CREAT | O_TRUNC);
    if (!file) return false;

    serializeJsonPretty(doc, file);
    file.close();
    return true;
}
