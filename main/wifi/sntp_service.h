#ifndef SNTP_SERVICE_H
#define SNTP_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

void sntp_service_start(const char *ntp_server);
void sntp_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SNTP_SERVICE_H */