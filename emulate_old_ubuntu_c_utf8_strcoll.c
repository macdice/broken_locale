#include "postgres.h"

#include "fmgr.h"
#include "utils/pg_locale.h"

#include <langinfo.h>
#include <limits.h>

struct broken_strcoll_range
{
	uint32_t codepoints[2];
	int order;
};

static const struct broken_strcoll_range broken_strcoll_table[] = {
#include "codepoint_table.h"
};

/* Search for the range covering 'codepoint' and return its order. */
static int
lookup(char32_t codepoint)
{
    int low = 0;
    int high = lengthof(broken_strcoll_table);
    int mid = high / 2;
    while (low <= high)
    {
		const struct broken_strcoll_range *range;

        mid = (low + high) / 2;
		range = &broken_strcoll_table[mid];

		if (range->codepoints[0] <= codepoint &&
			range->codepoints[1] < codepoint)
			low = mid + 1;
		else if (range->codepoints[0] > codepoint)
			high = mid - 1;
		else
		{
			Assert(range->codepoints[0] <= codepoint);
			Assert(range->codepoints[1] >= codepoint);
			return range->order + (codepoint - range->codepoints[0]);
		}
    }
    return INT_MAX;
}

/* A function to replace the standard collation function. */
static int
broken_strncoll(const char *begin1, ssize_t maybe_size1,
				const char *begin2, ssize_t maybe_size2,
				pg_locale_t locale)
{
	const size_t size1 = maybe_size1 < 0 ? strlen(begin1) : maybe_size1;
	const size_t size2 = maybe_size2 < 0 ? strlen(begin2) : maybe_size2;
	const char *end1 = begin1 + size1;
	const char *end2 = begin2 + size2;

	/* Compare using our order lookup table. */
	while (begin1 < end1 && begin2 < end2)
	{
		int mblen1 = pg_mblen_range(begin1, end1);
		int mblen2 = pg_mblen_range(begin2, end2);
		int order1 = lookup(utf8_to_unicode((const unsigned char *) begin1));
		int order2 = lookup(utf8_to_unicode((const unsigned char *) begin2));

		if (order1 < order2)
			return -1;
		else if (order1 > order2)
			return 1;

		begin1 += mblen1;
		begin2 += mblen2;
	}

	/* Equal so far.  Tie-break using length. */
	if (begin2 < end2)
		return -1;
	else if (begin1 < end1)
		return 1;

	/* Equal. */
	return 0;
}

PG_MODULE_MAGIC_EXT(
                    .name = "emulate_old_ubuntu_c_utf8_strcoll",
                    .version = PG_VERSION
);

void
_PG_init(void)
{
	static struct collate_methods patched_collate_methods;
	pg_locale_t locale = pg_database_locale();

	/*
	 * Use available clues to detect the libc provider (builtin wouldn't set
	 * collate, and ICU would set strxfrm_is_safe).
	 */
	if (locale->collate && !locale->collate->strxfrm_is_safe)
	{
		const char *locale_name;

		/* A glibc kludge to extract the name (getlocalename_l() is too new). */
		locale_name = nl_langinfo_l(NL_LOCALE_NAME(LC_COLLATE), locale->lt);

		/* Name is case-insensitive, with or without hyphen. */
		if (strcasecmp(locale_name, "C.UTF-8") == 0 ||
			strcasecmp(locale_name, "C.UTF8") == 0)
		{
			/*
			 * Photocopy its internal function table, replace the strncoll
			 * pointer with our own, and inject it back into the locale.  It
			 * remains in place for the lifetime of this backend.
			 */
			patched_collate_methods = *locale->collate;
			patched_collate_methods.strncoll = broken_strncoll;
			locale->collate = &patched_collate_methods;

			elog(LOG, "emulate_old_ubuntu_c_utf8_strcoll.so: active for default locale \"%s\"", locale_name);
		}
	}
}
