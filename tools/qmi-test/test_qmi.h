/*
 * test_qmi.h -- what the pieces of the QMI helper tests share: the kernel headers under test,
 * the counters and the check helpers (test_qmi.c defines them), and the suites' entry points
 * (test_encode.c, test_router.c).
 */

#ifndef TEST_QMI_H
#define TEST_QMI_H

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/annibale_qmi.h>
#include <sys/annibale_qrtr.h>

#define HZ 250                          /* what a Linux might have: qmi_txn_wait() must take it from here */
#define msecs_to_jiffies(ms)    ((unsigned long)(ms) * HZ / 1000)

extern int checks, failures;

void check(bool ok, const char *format, ...);
void check_bytes(const uint8_t *got, int got_length, const uint8_t *want, size_t want_length, const char *what);
uint64_t now_ms(void);

void test_own_tables(void);             /* test_encode.c  -- the coding against bytes written by hand */
void test_helpers(void);                /* test_router.c  -- the helpers over a router made up here */

#endif /* TEST_QMI_H */
