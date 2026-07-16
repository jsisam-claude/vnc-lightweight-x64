/*
 * ftpath_test — exhaustive checks for file-transfer name sanitization (the
 * path-traversal defense). Runs under the ASan/UBSan preset.
 */
#include <stdio.h>
#include <string.h>

#include "core/ftpath.h"

static int failures;

static void accept_ok(const char *in, const char *expect)
{
    char out[300];
    bool ok = ft_sanitize_remote_name(in, out, sizeof(out));
    int good = ok && strcmp(out, expect) == 0;
    printf("  accept %-28s -> %-20s %s\n", in, ok ? out : "(rejected)",
           good ? "ok" : "FAIL");
    if (!good) failures++;
}

static void reject(const char *in)
{
    char out[300];
    bool ok = ft_sanitize_remote_name(in, out, sizeof(out));
    int good = !ok && out[0] == '\0';
    printf("  reject %-28s %s\n", in, good ? "ok" : "FAIL");
    if (!good) failures++;
}

int main(void)
{
    printf("ftpath sanitizer tests:\n");

    /* Legitimate names. */
    accept_ok("report.pdf", "report.pdf");
    accept_ok("My File (1).txt", "My File (1).txt");
    accept_ok("archive.tar.gz", "archive.tar.gz");
    accept_ok(".config", ".config");

    /* Path traversal. */
    reject("..");
    reject(".");
    reject("../etc/passwd");
    reject("..\\..\\windows\\system32");
    reject("foo/bar.txt");
    reject("foo\\bar.txt");
    reject("/abs/path");
    reject("C:\\Windows\\x.txt");
    reject("C:relative");

    /* Alternate data streams / drive markers. */
    reject("file.txt:stream");

    /* Windows reserved device names (with and without extension). */
    reject("CON");
    reject("con");
    reject("PRN.txt");
    reject("aux");
    reject("NUL");
    reject("COM1");
    reject("lpt9.dat");

    /* Trailing dot/space, control chars, forbidden chars. */
    reject("name.");
    reject("name ");
    reject("bad\tname");
    reject("wild*card");
    reject("q?mark");
    reject("pipe|d");

    /* Bounds. */
    reject("");
    {
        char big[400];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        reject(big);
    }

    /* COM0 / LPT0 are NOT reserved -> accepted. */
    accept_ok("COM0", "COM0");
    accept_ok("comedy.txt", "comedy.txt"); /* not a device name */

    /* Basename extraction. */
    {
        int ok = strcmp(ft_basename("/a/b/c.txt"), "c.txt") == 0 &&
                 strcmp(ft_basename("C:\\d\\e.bin"), "e.bin") == 0 &&
                 strcmp(ft_basename("plain"), "plain") == 0;
        printf("  ft_basename %s\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
