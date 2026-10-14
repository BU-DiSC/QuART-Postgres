/*-------------------------------------------------------------------------
 *
 * elog_shim.c
 *		Minimal error-reporting stubs so replay links against the backend
 *		memory-context objects without the rest of the backend.  Any ERROR
 *		or failed assertion aborts the process.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "miscadmin.h"

bool
errstart(int elevel, const char *domain)
{
	return elevel >= ERROR;
}

bool
errstart_cold(int elevel, const char *domain)
{
	return elevel >= ERROR;
}

void
errfinish(const char *filename, int lineno, const char *funcname)
{
	fprintf(stderr, "ERROR at %s:%d\n", filename, lineno);
	abort();
}

int
errcode(int sqlerrcode)
{
	return 0;
}

int
errmsg(const char *fmt,...)
{
	return 0;
}

int
errmsg_internal(const char *fmt,...)
{
	return 0;
}

int
errdetail(const char *fmt,...)
{
	return 0;
}

int
errhint(const char *fmt,...)
{
	return 0;
}

int
errcode_for_file_access(void)
{
	return 0;
}

int
errhidestmt(bool hide_stmt)
{
	return 0;
}

int
errhidecontext(bool hide_ctx)
{
	return 0;
}

void
ExceptionalCondition(const char *conditionName, const char *fileName,
					 int lineNumber)
{
	fprintf(stderr, "Assert(%s) failed at %s:%d\n",
			conditionName, fileName, lineNumber);
	abort();
}

int
pg_mbcliplen(const char *mbstr, int len, int limit)
{
	return len < limit ? len : limit;
}

bool
stack_is_too_deep(void)
{
	return false;
}

void
pg_re_throw(void)
{
	abort();
}

volatile sig_atomic_t InterruptPending = 0;
volatile sig_atomic_t LogMemoryContextPending = 0;
sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;
int			MyProcPid = 0;
