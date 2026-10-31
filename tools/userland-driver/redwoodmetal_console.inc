/*
 * The compiler library and Mesa print with plain printf, and in a boot with three Metal
 * clients their "perf" and "rwa" lines could not be told apart. rw_console_init (a
 * constructor) puts a stream in front of stdout that starts each of those lines with
 * "[progname pid] "; this driver's own lines go to the stream behind it with their own prefix.
 * stdio calls the write function with the stream locked, so the line buffer needs no lock.
 */
#include <sys/stat.h>

static FILE *rw_console;

/* A line written here goes into the kernel log through kern.redwood.say. It replaces the console
 * in a process whose standard output is /dev/null, an ExtensionKit extension such as
 * MercuryPosterExtension, where every line of this driver and of the library was lost. The kernel
 * keeps at most 200 said lines a second, and a REDWOOD_THUMBS sample prints 33 lines at once, so
 * each line waits 5 ms. */
static int
rw_say_write(void *cookie, const char *bytes, int count)
{
	static char line[256];
	static size_t length;

	(void)cookie;
	for (int i = 0; i < count; i++) {
		if (bytes[i] != '\n')
			line[length++] = bytes[i];
		if (bytes[i] == '\n' || length == sizeof(line) - 1) {
			if (length > 0) {
				sysctlbyname("kern.redwood.say", NULL, NULL, line, length);
				usleep(5000);
			}
			length = 0;
		}
	}
	return count;
}

static int
rw_console_write(void *cookie, const char *bytes, int count)
{
	static char line[1024], who[64];
	static size_t length;

	(void)cookie;
	if (who[0] == '\0')
		snprintf(who, sizeof(who), "[%s %d] ", getprogname(), getpid());
	for (int i = 0; i < count; i++) {
		if (length < sizeof(line) - 1)
			line[length++] = bytes[i];
		if (bytes[i] != '\n' && length < sizeof(line) - 1)
			continue;
		fputs(who, rw_console);
		fwrite(line, 1, length, rw_console);
		if (line[length - 1] != '\n')
			fputc('\n', rw_console);
		fflush(rw_console);
		length = 0;
	}
	return count;
}

__attribute__((constructor)) static void
rw_console_init(void)
{
	FILE *prefixed = funopen(NULL, NULL, rw_console_write, NULL, NULL);

	if (prefixed == NULL)
		return;
	setvbuf(prefixed, NULL, _IOLBF, 0);
	rw_console = stdout;
	stdout = prefixed;

	struct stat out, null;
	FILE *said;

	if (fstat(STDOUT_FILENO, &out) == 0 && stat("/dev/null", &null) == 0 && S_ISCHR(out.st_mode) &&
	    out.st_rdev == null.st_rdev && (said = funopen(NULL, NULL, rw_say_write, NULL, NULL)) != NULL) {
		setvbuf(said, NULL, _IOLBF, 0);
		rw_console = said;
	}
}
