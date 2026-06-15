// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Tests for nfs4_recovery_build_tag(), which assembles the recovery tag stored
 * in the grace-period client list.  The tag format is "IP-(len:opaque)".
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "sal_functions.h"
#include "abstract_mem.h"

static int failures;

#define CHECK(cond, fmt, ...)                                               \
	do {                                                                \
		if (!(cond)) {                                              \
			printf("FAIL %s:%d: " fmt "\n", __func__, __LINE__, \
			       ##__VA_ARGS__);                              \
			failures++;                                         \
		}                                                           \
	} while (0)

static void test_with_ip_printable(void)
{
	/* Printable opaque value: tag should be "IP-(len:opaque)" */
	char *tag = nfs4_recovery_build_tag("192.168.1.10",
					    "Linux NFSv4.1 client", 20);

	CHECK(tag != NULL, "tag is NULL");
	CHECK(strcmp(tag, "192.168.1.10-(20:Linux NFSv4.1 client)") == 0,
	      "got [%s]", tag);
	gsh_free(tag);
}

static void test_with_ip_single_char(void)
{
	char *tag = nfs4_recovery_build_tag("10.0.0.1", "A", 1);

	CHECK(tag != NULL, "tag is NULL");
	CHECK(strcmp(tag, "10.0.0.1-(1:A)") == 0, "got [%s]", tag);
	gsh_free(tag);
}

static void test_with_ipv6(void)
{
	char *tag = nfs4_recovery_build_tag("fe80::1", "client", 6);

	CHECK(tag != NULL, "tag is NULL");
	CHECK(strcmp(tag, "fe80::1-(6:client)") == 0, "got [%s]", tag);
	gsh_free(tag);
}

int main(void)
{
	test_with_ip_printable();
	test_with_ip_single_char();
	test_with_ipv6();

	if (failures)
		printf("%d test(s) FAILED\n", failures);
	else
		printf("All tests passed\n");

	return failures ? 1 : 0;
}
