/*
 * Tests of the QMI helpers (bsd/dev/annibale_qmi.c in the kernel tree) on the machine that builds
 * them; tools/qmi-test/run.sh compiles and runs this.
 */

#include "test_qmi.h"

int checks, failures;

void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

void
check_bytes(const uint8_t *got, int got_length, const uint8_t *want, size_t want_length, const char *what)
{
	bool same = got_length == (int)want_length && memcmp(got, want, want_length) == 0;

	check(same, "%s: the encoded message is not the expected one", what);
	if (!same) {
		printf("  got  (%d):", got_length);
		for (int i = 0; i < got_length && i < 80; i++) {
			printf(" %02x", got[i]);
		}
		printf("\n  want (%zu):", want_length);
		for (size_t i = 0; i < want_length && i < 80; i++) {
			printf(" %02x", want[i]);
		}
		printf("\n");
	}
}

uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int
main(void)
{
	static const struct {
		const char *name;
		void (*run)(void);
	} parts[] = {
		{ "the coding, tables of our own, bytes by hand", test_own_tables },
		{ "the helpers over a made-up router", test_helpers },
	};

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		int before = failures, done = checks;

		parts[i].run();
		printf("%s  %s (%d checks)\n", failures == before ? "ok  " : "FAIL", parts[i].name, checks - done);
	}
	printf("%d checks, %d failed\n", checks, failures);
	return failures != 0;
}
