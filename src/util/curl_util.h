#ifndef CINDER_CURL_UTIL_H
#define CINDER_CURL_UTIL_H

#include <curl/curl.h>

/* Apply secure SSL + networking options to a CURL handle.
 * On Windows: uses the native Windows certificate store.
 * On other platforms: uses the system CA bundle. */
static inline void cinder_curl_ssl_setup(CURL *curl) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  /* fail fast on bad hosts */
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);        /* guard against redirect loops */
#ifdef _WIN32
    /* Use Windows native certificate store — no CA bundle file needed */
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
}

#endif /* CINDER_CURL_UTIL_H */
