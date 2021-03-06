#include <ccan/tdb/tdb.h>
#include <ccan/grab_file/grab_file.h>
#include <ccan/hash/hash.h>
#include <ccan/talloc/talloc.h>
#include <ccan/str_talloc/str_talloc.h>
#include <ccan/str/str.h>
#include <ccan/list/list.h>
#include <err.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <fcntl.h>

#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)

static bool quiet = false;

/* Avoid mod by zero */
static unsigned int total_keys = 1;

/* All the wipe_all ops. */
static struct op_desc *wipe_alls = NULL;
static unsigned int num_wipe_alls = 0;

/* #define DEBUG_DEPS 1 */

/* Traversals block transactions in the current implementation. */
#define TRAVERSALS_TAKE_TRANSACTION_LOCK 1

struct pipe {
	int fd[2];
};
static struct pipe *pipes;
static int backoff_fd = -1;

static void __attribute__((noreturn)) fail(const char *filename,
					   unsigned int line,
					   const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s:%u: FAIL: ", filename, line);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}
	
/* Try or die. */
#define try(expr, expect)						\
	do {								\
		int ret = (expr);					\
		if (ret != (expect))					\
			fail(filename[file], i+1,			\
			     STRINGIFY(expr) "= %i", ret);		\
	} while (0)

/* Try or imitate results. */
#define unreliable(expr, expect, force, undo)				\
	do {								\
		int ret = expr;						\
		if (ret != expect) {					\
			fprintf(stderr, "%s:%u: %s gave %i not %i",	\
				filename[file], i+1, STRINGIFY(expr),	\
				ret, expect);				\
			if (expect == 0)				\
				force;					\
			else						\
				undo;					\
		}							\
	} while (0)

static bool key_eq(TDB_DATA a, TDB_DATA b)
{
	if (a.dsize != b.dsize)
		return false;
	return memcmp(a.dptr, b.dptr, a.dsize) == 0;
}

/* This is based on the hash algorithm from gdbm */
static unsigned int hash_key(TDB_DATA *key)
{
	uint32_t value;	/* Used to compute the hash value.  */
	uint32_t   i;	/* Used to cycle through random values. */

	/* Set the initial value from the key size. */
	for (value = 0x238F13AF ^ key->dsize, i=0; i < key->dsize; i++)
		value = (value + (key->dptr[i] << (i*5 % 24)));

	return (1103515243 * value + 12345);  
}

enum op_type {
	OP_TDB_LOCKALL,
	OP_TDB_LOCKALL_MARK,
	OP_TDB_LOCKALL_UNMARK,
	OP_TDB_LOCKALL_NONBLOCK,
	OP_TDB_UNLOCKALL,
	OP_TDB_LOCKALL_READ,
	OP_TDB_LOCKALL_READ_NONBLOCK,
	OP_TDB_UNLOCKALL_READ,
	OP_TDB_CHAINLOCK,
	OP_TDB_CHAINLOCK_NONBLOCK,
	OP_TDB_CHAINLOCK_MARK,
	OP_TDB_CHAINLOCK_UNMARK,
	OP_TDB_CHAINUNLOCK,
	OP_TDB_CHAINLOCK_READ,
	OP_TDB_CHAINUNLOCK_READ,
	OP_TDB_PARSE_RECORD,
	OP_TDB_EXISTS,
	OP_TDB_STORE,
	OP_TDB_APPEND,
	OP_TDB_GET_SEQNUM,
	OP_TDB_WIPE_ALL,
	OP_TDB_TRANSACTION_START,
	OP_TDB_TRANSACTION_CANCEL,
	OP_TDB_TRANSACTION_PREPARE_COMMIT,
	OP_TDB_TRANSACTION_COMMIT,
	OP_TDB_TRAVERSE_READ_START,
	OP_TDB_TRAVERSE_START,
	OP_TDB_TRAVERSE_END,
	OP_TDB_TRAVERSE,
	OP_TDB_TRAVERSE_END_EARLY,
	OP_TDB_FIRSTKEY,
	OP_TDB_NEXTKEY,
	OP_TDB_FETCH,
	OP_TDB_DELETE,
	OP_TDB_REPACK,
};

struct op {
	unsigned int seqnum;
	enum op_type type;
	TDB_DATA key;
	TDB_DATA data;
	int ret;

	/* Who is waiting for us? */
	struct list_head post;
	/* What are we waiting for? */
	struct list_head pre;

	/* If I'm part of a group (traverse/transaction) where is
	 * start?  (Otherwise, 0) */
	unsigned int group_start;

	union {
		int flag; /* open and store */
		struct {  /* append */
			TDB_DATA pre;
			TDB_DATA post;
		} append;
		/* transaction/traverse start/chainlock */
		unsigned int group_len;
	};
};

struct op_desc {
	unsigned int file;
	unsigned int op_num;
};

static unsigned char hex_char(const char *filename, unsigned int line, char c)
{
	c = toupper(c);
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= '0' && c <= '9')
		return c - '0';
	fail(filename, line, "invalid hex character '%c'", c);
}

/* TDB data is <size>:<%02x>* */
static TDB_DATA make_tdb_data(const void *ctx,
			      const char *filename, unsigned int line,
			      const char *word)
{
	TDB_DATA data;
	unsigned int i;
	const char *p;

	if (streq(word, "NULL"))
		return tdb_null;

	data.dsize = atoi(word);
	data.dptr = talloc_array(ctx, unsigned char, data.dsize);
	p = strchr(word, ':');
	if (!p)
		fail(filename, line, "invalid tdb data '%s'", word);
	p++;
	for (i = 0; i < data.dsize; i++)
		data.dptr[i] = hex_char(filename, line, p[i*2])*16
			+ hex_char(filename, line, p[i*2+1]);

	return data;
}

static void add_op(const char *filename, struct op **op, unsigned int i,
		   unsigned int seqnum, enum op_type type)
{
	struct op *new;
	*op = talloc_realloc(NULL, *op, struct op, i+1);
	new = (*op) + i;
	new->type = type;
	new->seqnum = seqnum;
	new->ret = 0;
	new->group_start = 0;
}

static void op_add_nothing(char *filename[], struct op op[],
			   unsigned file, unsigned op_num, char *words[])
{
	if (words[2])
		fail(filename[file], op_num+1, "Expected no arguments");
	op[op_num].key = tdb_null;
}

static void op_add_key(char *filename[], struct op op[],
		       unsigned file, unsigned op_num, char *words[])
{
	if (words[2] == NULL || words[3])
		fail(filename[file], op_num+1, "Expected just a key");

	op[op_num].key = make_tdb_data(op, filename[file], op_num+1, words[2]);
	total_keys++;
}

static void op_add_key_ret(char *filename[], struct op op[],
			   unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || !words[4] || words[5]
	    || !streq(words[3], "="))
		fail(filename[file], op_num+1, "Expected <key> = <ret>");
	op[op_num].ret = atoi(words[4]);
	op[op_num].key = make_tdb_data(op, filename[file], op_num+1, words[2]);
	/* May only be a unique key if it fails */
	if (op[op_num].ret != 0)
		total_keys++;
}

static void op_add_key_data(char *filename[], struct op op[],
			    unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || !words[4] || words[5]
	    || !streq(words[3], "="))
		fail(filename[file], op_num+1, "Expected <key> = <data>");
	op[op_num].key = make_tdb_data(op, filename[file], op_num+1, words[2]);
	op[op_num].data = make_tdb_data(op, filename[file], op_num+1, words[4]);
	/* Likely only be a unique key if it fails */
	if (!op[op_num].data.dptr)
		total_keys++;
	else if (random() % 2)
		total_keys++;
}

/* We don't record the keys or data for a traverse, as we don't use them. */
static void op_add_traverse(char *filename[], struct op op[],
			    unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || !words[4] || words[5]
	    || !streq(words[3], "="))
		fail(filename[file], op_num+1, "Expected <key> = <data>");
	op[op_num].key = tdb_null;
}

/* Full traverse info is useful for debugging, but changing it to
 * "traversefn" without the data makes the traces *much* smaller! */
static void op_add_traversefn(char *filename[], struct op op[],
			    unsigned file, unsigned op_num, char *words[])
{
	if (words[2])
		fail(filename[file], op_num+1, "Expected no values");
	op[op_num].key = tdb_null;
}

/* <seqnum> tdb_store <rec> <rec> <flag> = <ret> */
static void op_add_store(char *filename[], struct op op[],
			 unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || !words[4] || !words[5] || !words[6]
	    || words[7] || !streq(words[5], "="))
		fail(filename[file], op_num+1, "Expect <key> <data> <flag> = <ret>");

	op[op_num].flag = strtoul(words[4], NULL, 0);
	op[op_num].ret = atoi(words[6]);
	op[op_num].key = make_tdb_data(op, filename[file], op_num+1, words[2]);
	op[op_num].data = make_tdb_data(op, filename[file], op_num+1, words[3]);
	total_keys++;
}

/* <seqnum> tdb_append <rec> <rec> = <rec> */
static void op_add_append(char *filename[], struct op op[],
			  unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || !words[4] || !words[5] || words[6]
	    || !streq(words[4], "="))
		fail(filename[file], op_num+1, "Expect <key> <data> = <rec>");

	op[op_num].key = make_tdb_data(op, filename[file], op_num+1, words[2]);
	op[op_num].data = make_tdb_data(op, filename[file], op_num+1, words[3]);

	op[op_num].append.post
		= make_tdb_data(op, filename[file], op_num+1, words[5]);

	/* By subtraction, figure out what previous data was. */
	op[op_num].append.pre.dptr = op[op_num].append.post.dptr;
	op[op_num].append.pre.dsize
		= op[op_num].append.post.dsize - op[op_num].data.dsize;
	total_keys++;
}

/* <seqnum> tdb_get_seqnum = <ret> */
static void op_add_seqnum(char *filename[], struct op op[],
			  unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || words[4] || !streq(words[2], "="))
		fail(filename[file], op_num+1, "Expect = <ret>");

	op[op_num].key = tdb_null;
	op[op_num].ret = atoi(words[3]);
}

static void op_add_traverse_start(char *filename[], struct op op[],
				  unsigned file, unsigned op_num, char *words[])
{
	if (words[2])
		fail(filename[file], op_num+1, "Expect no arguments");

	op[op_num].key = tdb_null;
	op[op_num].group_len = 0;
}

static void op_add_transaction(char *filename[], struct op op[],
			       unsigned file, unsigned op_num, char *words[])
{
	if (words[2])
		fail(filename[file], op_num+1, "Expect no arguments");

	op[op_num].key = tdb_null;
	op[op_num].group_len = 0;
}

static void op_add_chainlock(char *filename[], struct op op[],
			     unsigned file, unsigned op_num, char *words[])
{
	if (words[2] == NULL || words[3])
		fail(filename[file], op_num+1, "Expected just a key");

	/* A chainlock key isn't a key in the normal sense; it doesn't
	 * have to be in the db at all.  Also, we don't want to hash this op. */
	op[op_num].data = make_tdb_data(op, filename[file], op_num+1, words[2]);
	op[op_num].key = tdb_null;
	op[op_num].group_len = 0;
}

static void op_add_chainlock_ret(char *filename[], struct op op[],
				 unsigned file, unsigned op_num, char *words[])
{
	if (!words[2] || !words[3] || !words[4] || words[5]
	    || !streq(words[3], "="))
		fail(filename[file], op_num+1, "Expected <key> = <ret>");
	op[op_num].ret = atoi(words[4]);
	op[op_num].data = make_tdb_data(op, filename[file], op_num+1, words[2]);
	op[op_num].key = tdb_null;
	op[op_num].group_len = 0;
	total_keys++;
}

static void op_add_wipe_all(char *filename[], struct op op[],
			    unsigned file, unsigned op_num, char *words[])
{
	if (words[2])
		fail(filename[file], op_num+1, "Expected no arguments");
	op[op_num].key = tdb_null;
	wipe_alls = talloc_realloc(NULL, wipe_alls, struct op_desc,
				   num_wipe_alls+1);
	wipe_alls[num_wipe_alls].file = file;
	wipe_alls[num_wipe_alls].op_num = op_num;
	num_wipe_alls++;
}

static int op_find_start(struct op op[], unsigned int op_num, enum op_type type)
{
	unsigned int i;

	for (i = op_num-1; i > 0; i--) {
		if (op[i].type == type && !op[i].group_len)
			return i;
	}
	return 0;
}

static void op_analyze_transaction(char *filename[], struct op op[],
				   unsigned file, unsigned op_num,
				   char *words[])
{
	unsigned int start, i;

	op[op_num].key = tdb_null;

	if (words[2])
		fail(filename[file], op_num+1, "Expect no arguments");

	start = op_find_start(op, op_num, OP_TDB_TRANSACTION_START);
	if (!start)
		fail(filename[file], op_num+1, "no transaction start found");

	op[start].group_len = op_num - start;

	/* This rolls in nested transactions.  I think that's right. */
	for (i = start; i <= op_num; i++)
		op[i].group_start = start;
}

/* We treat chainlocks a lot like transactions, even though that's overkill */
static void op_analyze_chainlock(char *filename[], struct op op[],
				 unsigned file, unsigned op_num, char *words[])
{
	unsigned int i, start;

	if (words[2] == NULL || words[3])
		fail(filename[file], op_num+1, "Expected just a key");

	op[op_num].data = make_tdb_data(op, filename[file], op_num+1, words[2]);
	op[op_num].key = tdb_null;
	total_keys++;

	start = op_find_start(op, op_num, OP_TDB_CHAINLOCK);
	if (!start)
		start = op_find_start(op, op_num, OP_TDB_CHAINLOCK_READ);
	if (!start)
		fail(filename[file], op_num+1, "no initial chainlock found");

	/* FIXME: We'd have to do something clever to make this work
	 * vs. deadlock. */
	if (!key_eq(op[start].data, op[op_num].data))
		fail(filename[file], op_num+1, "nested chainlock calls?");

	op[start].group_len = op_num - start;
	for (i = start; i <= op_num; i++)
		op[i].group_start = start;
}

static void op_analyze_traverse(char *filename[], struct op op[],
				unsigned file, unsigned op_num, char *words[])
{
	int i, start;

	op[op_num].key = tdb_null;

	/* = %u means traverse function terminated. */
	if (words[2]) {
		if (!streq(words[2], "=") || !words[3] || words[4])
			fail(filename[file], op_num+1, "expect = <num>");
		op[op_num].ret = atoi(words[3]);
	} else
		op[op_num].ret = 0;

	start = op_find_start(op, op_num, OP_TDB_TRAVERSE_START);
	if (!start)
		start = op_find_start(op, op_num, OP_TDB_TRAVERSE_READ_START);
	if (!start)
		fail(filename[file], op_num+1, "no traversal start found");

	op[start].group_len = op_num - start;

	/* Don't roll in nested traverse/chainlock */
	for (i = start; i <= op_num; i++)
		if (!op[i].group_start)
			op[i].group_start = start;
}

/* Keep -Wmissing-declarations happy: */
const struct op_table *
find_keyword (register const char *str, register unsigned int len);

#include "keywords.c"

struct depend {
	/* We can have more than one */
	struct list_node pre_list;
	struct list_node post_list;
	struct op_desc needs;
	struct op_desc prereq;
};

static void check_deps(const char *filename, struct op op[], unsigned int num)
{
#ifdef DEBUG_DEPS
	unsigned int i;

	for (i = 1; i < num; i++)
		if (!list_empty(&op[i].pre))
			fail(filename, i+1, "Still has dependencies");
#endif
}

static void dump_pre(char *filename[], struct op *op[],
		     unsigned int file, unsigned int i)
{
	struct depend *dep;

	if (!quiet) {
		printf("%s:%u (%u) still waiting for:\n", filename[file], i+1,
		       op[file][i].seqnum);
		list_for_each(&op[file][i].pre, dep, pre_list)
			printf("    %s:%u (%u)\n",
			       filename[dep->prereq.file], dep->prereq.op_num+1,
			       op[dep->prereq.file][dep->prereq.op_num].seqnum);
	}
	check_deps(filename[file], op[file], i);
}

/* We simply read/write pointers, since we all are children. */
static bool do_pre(struct tdb_context *tdb,
		   char *filename[], struct op *op[],
		   unsigned int file, int pre_fd, unsigned int i,
		   bool backoff)
{
	while (!list_empty(&op[file][i].pre)) {
		struct depend *dep;

#if DEBUG_DEPS
		printf("%s:%u:waiting for pre\n", filename[file], i+1);
		fflush(stdout);
#endif
		if (backoff)
			alarm(2);
		else
			alarm(10);
		while (read(pre_fd, &dep, sizeof(dep)) != sizeof(dep)) {
			if (errno == EINTR) {
				if (backoff) {
					struct op_desc desc = { file,i };
					warnx("%s:%u:avoiding deadlock",
					      filename[file], i+1);
					if (write(backoff_fd, &desc,
						  sizeof(desc)) != sizeof(desc))
						err(1, "writing backoff_fd");
					return false;
				}
				dump_pre(filename, op, file, i);
				exit(1);
			} else
				errx(1, "Reading from pipe");
		}
		alarm(0);

#if DEBUG_DEPS
		printf("%s:%u:got pre %u from %s:%u\n", filename[file], i+1,
		       dep->needs.op_num+1, filename[dep->prereq.file],
		       dep->prereq.op_num+1);
		fflush(stdout);
#endif
		/* This could be any op, not just this one. */
		talloc_free(dep);
	}
	return true;
}

static void do_post(char *filename[], struct op *op[],
		    unsigned int file, unsigned int i)
{
	struct depend *dep;

	list_for_each(&op[file][i].post, dep, post_list) {
#if DEBUG_DEPS
		printf("%s:%u:sending to file %s:%u\n", filename[file], i+1,
		       filename[dep->needs.file], dep->needs.op_num+1);
#endif
		if (write(pipes[dep->needs.file].fd[1], &dep, sizeof(dep))
		    != sizeof(dep))
			err(1, "%s:%u failed to tell file %s",
			    filename[file], i+1, filename[dep->needs.file]);
	}
}

static int get_len(TDB_DATA key, TDB_DATA data, void *private_data)
{
	return data.dsize;
}

static unsigned run_ops(struct tdb_context *tdb,
			int pre_fd,
			char *filename[],
			struct op *op[],
			unsigned int file,
			unsigned int start, unsigned int stop,
			bool backoff);

struct traverse_info {
	struct op **op;
	char **filename;
	unsigned file;
	int pre_fd;
	unsigned int start;
	unsigned int i;
};

/* More complex.  Just do whatever's they did at the n'th entry. */
static int nontrivial_traverse(struct tdb_context *tdb,
			       TDB_DATA key, TDB_DATA data,
			       void *_tinfo)
{
	struct traverse_info *tinfo = _tinfo;
	unsigned int trav_len = tinfo->op[tinfo->file][tinfo->start].group_len;
	bool avoid_deadlock = false;

	if (tinfo->i == tinfo->start + trav_len) {
		/* This can happen if traverse expects to be empty. */
		if (trav_len == 1)
			return 1;
		fail(tinfo->filename[tinfo->file], tinfo->start + 1,
		     "traverse did not terminate");
	}

	if (tinfo->op[tinfo->file][tinfo->i].type != OP_TDB_TRAVERSE)
		fail(tinfo->filename[tinfo->file], tinfo->start + 1,
		     "%s:%u:traverse terminated early");

#if TRAVERSALS_TAKE_TRANSACTION_LOCK
	avoid_deadlock = true;
#endif

	/* Run any normal ops. */
	tinfo->i = run_ops(tdb, tinfo->pre_fd, tinfo->filename, tinfo->op,
			   tinfo->file, tinfo->i+1, tinfo->start + trav_len,
			   avoid_deadlock);

	/* We backed off, or we hit OP_TDB_TRAVERSE_END/EARLY. */
	if (tinfo->op[tinfo->file][tinfo->i].type != OP_TDB_TRAVERSE)
		return 1;

	return 0;
}

static unsigned op_traverse(struct tdb_context *tdb,
			    int pre_fd,
			    char *filename[],
			    unsigned int file,
			    int (*traversefn)(struct tdb_context *,
					      tdb_traverse_func, void *),
			    struct op *op[],
			    unsigned int start)
{
	struct traverse_info tinfo = { op, filename, file, pre_fd,
				       start, start+1 };

	traversefn(tdb, nontrivial_traverse, &tinfo);

	/* Traversing in wrong order can have strange effects: eg. if
	 * original traverse went A (delete A), B, we might do B
	 * (delete A).  So if we have ops left over, we do it now. */
	while (tinfo.i != start + op[file][start].group_len) {
		if (op[file][tinfo.i].type == OP_TDB_TRAVERSE
		    || op[file][tinfo.i].type == OP_TDB_TRAVERSE_END_EARLY)
			tinfo.i++;
		else
			tinfo.i = run_ops(tdb, pre_fd, filename, op, file,
					  tinfo.i,
					  start + op[file][start].group_len,
					  false);
	}

	return tinfo.i;
}

static void break_out(int sig)
{
}

static __attribute__((noinline))
unsigned run_ops(struct tdb_context *tdb,
		 int pre_fd,
		 char *filename[],
		 struct op *op[],
		 unsigned int file,
		 unsigned int start, unsigned int stop,
		 bool backoff)
{
	unsigned int i;
	struct sigaction sa;

	sa.sa_handler = break_out;
	sa.sa_flags = 0;

	sigaction(SIGALRM, &sa, NULL);
	for (i = start; i < stop; i++) {
		if (!do_pre(tdb, filename, op, file, pre_fd, i, backoff))
			return i;

		switch (op[file][i].type) {
		case OP_TDB_LOCKALL:
			try(tdb_lockall(tdb), op[file][i].ret);
			break;
		case OP_TDB_LOCKALL_MARK:
			try(tdb_lockall_mark(tdb), op[file][i].ret);
			break;
		case OP_TDB_LOCKALL_UNMARK:
			try(tdb_lockall_unmark(tdb), op[file][i].ret);
			break;
		case OP_TDB_LOCKALL_NONBLOCK:
			unreliable(tdb_lockall_nonblock(tdb), op[file][i].ret,
				   tdb_lockall(tdb), tdb_unlockall(tdb));
			break;
		case OP_TDB_UNLOCKALL:
			try(tdb_unlockall(tdb), op[file][i].ret);
			break;
		case OP_TDB_LOCKALL_READ:
			try(tdb_lockall_read(tdb), op[file][i].ret);
			break;
		case OP_TDB_LOCKALL_READ_NONBLOCK:
			unreliable(tdb_lockall_read_nonblock(tdb),
				   op[file][i].ret,
				   tdb_lockall_read(tdb),
				   tdb_unlockall_read(tdb));
			break;
		case OP_TDB_UNLOCKALL_READ:
			try(tdb_unlockall_read(tdb), op[file][i].ret);
			break;
		case OP_TDB_CHAINLOCK:
			try(tdb_chainlock(tdb, op[file][i].key),
			    op[file][i].ret);
			break;
		case OP_TDB_CHAINLOCK_NONBLOCK:
			unreliable(tdb_chainlock_nonblock(tdb, op[file][i].key),
				   op[file][i].ret,
				   tdb_chainlock(tdb, op[file][i].key),
				   tdb_chainunlock(tdb, op[file][i].key));
			break;
		case OP_TDB_CHAINLOCK_MARK:
			try(tdb_chainlock_mark(tdb, op[file][i].key),
			    op[file][i].ret);
			break;
		case OP_TDB_CHAINLOCK_UNMARK:
			try(tdb_chainlock_unmark(tdb, op[file][i].key),
			    op[file][i].ret);
			break;
		case OP_TDB_CHAINUNLOCK:
			try(tdb_chainunlock(tdb, op[file][i].key),
			    op[file][i].ret);
			break;
		case OP_TDB_CHAINLOCK_READ:
			try(tdb_chainlock_read(tdb, op[file][i].key),
			    op[file][i].ret);
			break;
		case OP_TDB_CHAINUNLOCK_READ:
			try(tdb_chainunlock_read(tdb, op[file][i].key),
			    op[file][i].ret);
			break;
		case OP_TDB_PARSE_RECORD:
			try(tdb_parse_record(tdb, op[file][i].key, get_len,
					     NULL),
			    op[file][i].ret);
			break;
		case OP_TDB_EXISTS:
			try(tdb_exists(tdb, op[file][i].key), op[file][i].ret);
			break;
		case OP_TDB_STORE:
			try(tdb_store(tdb, op[file][i].key, op[file][i].data,
				      op[file][i].flag),
			    op[file][i].ret);
			break;
		case OP_TDB_APPEND:
			try(tdb_append(tdb, op[file][i].key, op[file][i].data),
			    op[file][i].ret);
			break;
		case OP_TDB_GET_SEQNUM:
			try(tdb_get_seqnum(tdb), op[file][i].ret);
			break;
		case OP_TDB_WIPE_ALL:
			try(tdb_wipe_all(tdb), op[file][i].ret);
			break;
		case OP_TDB_TRANSACTION_START:
			try(tdb_transaction_start(tdb), op[file][i].ret);
			break;
		case OP_TDB_TRANSACTION_CANCEL:
			try(tdb_transaction_cancel(tdb), op[file][i].ret);
			break;
		case OP_TDB_TRANSACTION_PREPARE_COMMIT:
			try(tdb_transaction_prepare_commit(tdb),
			    op[file][i].ret);
			break;
		case OP_TDB_TRANSACTION_COMMIT:
			try(tdb_transaction_commit(tdb), op[file][i].ret);
			break;
		case OP_TDB_TRAVERSE_READ_START:
			i = op_traverse(tdb, pre_fd, filename, file,
					tdb_traverse_read, op, i);
			break;
		case OP_TDB_TRAVERSE_START:
			i = op_traverse(tdb, pre_fd, filename, file,
					tdb_traverse, op, i);
			break;
		case OP_TDB_TRAVERSE:
		case OP_TDB_TRAVERSE_END_EARLY:
			/* Terminate: we're in a traverse, and we've
			 * done our ops. */
			return i;
		case OP_TDB_TRAVERSE_END:
			fail(filename[file], i+1, "unexpected end traverse");
		/* FIXME: These must be treated like traverse. */
		case OP_TDB_FIRSTKEY:
			if (!key_eq(tdb_firstkey(tdb), op[file][i].data))
				fail(filename[file], i+1, "bad firstkey");
			break;
		case OP_TDB_NEXTKEY:
			if (!key_eq(tdb_nextkey(tdb, op[file][i].key),
				    op[file][i].data))
				fail(filename[file], i+1, "bad nextkey");
			break;
		case OP_TDB_FETCH: {
			TDB_DATA f = tdb_fetch(tdb, op[file][i].key);
			if (!key_eq(f, op[file][i].data))
				fail(filename[file], i+1, "bad fetch %u",
				     f.dsize);
			break;
		}
		case OP_TDB_DELETE:
			try(tdb_delete(tdb, op[file][i].key), op[file][i].ret);
			break;
		case OP_TDB_REPACK:
			/* We do nothing here: the transaction and traverse are
			 * traced.  It's in the trace to mark it, since it
			 * may become unnecessary in future. */
			break;
		}
		do_post(filename, op, file, i);
	}
	return i;
}

/* tdbtorture, in particular, can do a tdb_close with a transaction in
 * progress. */
static struct op *maybe_cancel_transaction(char *filename[], unsigned int file,
					   struct op *op, unsigned int *num)
{
	unsigned int start = op_find_start(op, *num, OP_TDB_TRANSACTION_START);

	if (start) {
		char *words[] = { "<unknown>", "tdb_close", NULL };
		add_op(filename[file], &op, *num, op[start].seqnum,
		       OP_TDB_TRANSACTION_CANCEL);
		op_analyze_transaction(filename, op, file, *num, words);
		(*num)++;
	}
	return op;
}

static struct op *load_tracefile(char *filename[],
				 unsigned int file,
				 unsigned int *num,
				 unsigned int *hashsize,
				 unsigned int *tdb_flags,
				 unsigned int *open_flags)
{
	unsigned int i;
	struct op *op = talloc_array(NULL, struct op, 1);
	char **words;
	char **lines;
	char *contents;

	contents = grab_file(NULL, filename[file], NULL);
	if (!contents)
		err(1, "Reading %s", filename[file]);

	lines = strsplit(contents, contents, "\n");
	if (!lines[0])
		errx(1, "%s is empty", filename[file]);

	words = strsplit(lines, lines[0], " ");
	if (!streq(words[1], "tdb_open"))
		fail(filename[file], 1, "does not start with tdb_open");

	*hashsize = atoi(words[2]);
	*tdb_flags = strtoul(words[3], NULL, 0);
	*open_flags = strtoul(words[4], NULL, 0);

	for (i = 1; lines[i]; i++) {
		const struct op_table *opt;

		words = strsplit(lines, lines[i], " ");
		if (!words[0] || !words[1])
			fail(filename[file], i+1,
			     "Expected seqnum number and op");
	       
		opt = find_keyword(words[1], strlen(words[1]));
		if (!opt) {
			if (streq(words[1], "tdb_close")) {
				if (lines[i+1])
					fail(filename[file], i+2,
					     "lines after tdb_close");
				*num = i;
				talloc_free(lines);
				return maybe_cancel_transaction(filename, file,
								op, num);
			}
			fail(filename[file], i+1,
			     "Unknown operation '%s'", words[1]);
		}

		add_op(filename[file], &op, i, atoi(words[0]), opt->type);
		opt->enhance_op(filename, op, file, i, words);
	}

	if (!quiet)
		fprintf(stderr,
			"%s:%u:last operation is not tdb_close: incomplete?",
			filename[file], i);

	talloc_free(contents);
	*num = i - 1;
	return maybe_cancel_transaction(filename, file, op, num);
}

/* We remember all the keys we've ever seen, and who has them. */
struct keyinfo {
	TDB_DATA key;
	unsigned int num_users;
	struct op_desc *user;
};

static bool starts_transaction(const struct op *op)
{
	return op->type == OP_TDB_TRANSACTION_START;
}

static bool in_transaction(const struct op op[], unsigned int i)
{
	return op[i].group_start && starts_transaction(&op[op[i].group_start]);
}

static bool successful_transaction(const struct op *op)
{
	return starts_transaction(op)
		&& op[op->group_len].type == OP_TDB_TRANSACTION_COMMIT;
}

static bool starts_traverse(const struct op *op)
{
	return op->type == OP_TDB_TRAVERSE_START
		|| op->type == OP_TDB_TRAVERSE_READ_START;
}

static bool in_traverse(const struct op op[], unsigned int i)
{
	return op[i].group_start && starts_traverse(&op[op[i].group_start]);
}

static bool starts_chainlock(const struct op *op)
{
	return op->type == OP_TDB_CHAINLOCK_READ
		|| op->type == OP_TDB_CHAINLOCK;
}

static bool in_chainlock(const struct op op[], unsigned int i)
{
	return op[i].group_start && starts_chainlock(&op[op[i].group_start]);
}

static const TDB_DATA must_not_exist;
static const TDB_DATA must_exist;
static const TDB_DATA not_exists_or_empty;

/* NULL means doesn't care if it exists or not, &must_exist means
 * it must exist but we don't care what, &must_not_exist means it must
 * not exist, otherwise the data it needs. */
static const TDB_DATA *needs(const TDB_DATA *key, const struct op *op)
{
	/* Look through for an op in this transaction which needs this key. */
	if (starts_transaction(op) || starts_chainlock(op)) {
		unsigned int i;
		const TDB_DATA *need = NULL;

		for (i = 1; i < op->group_len; i++) {
			if (key_eq(op[i].key, *key)
			    || op[i].type == OP_TDB_WIPE_ALL) {
				need = needs(key, &op[i]);
				/* tdb_exists() is special: there might be
				 * something in the transaction with more
				 * specific requirements.  Other ops don't have
				 * specific requirements (eg. store or delete),
				 * but they change the value so we can't get
				 * more information from future ops. */
				if (op[i].type != OP_TDB_EXISTS)
					break;
			}
		}

		return need;
	}

	switch (op->type) {
	/* FIXME: Pull forward deps, since we can deadlock */
	case OP_TDB_CHAINLOCK:
	case OP_TDB_CHAINLOCK_NONBLOCK:
	case OP_TDB_CHAINLOCK_MARK:
	case OP_TDB_CHAINLOCK_UNMARK:
	case OP_TDB_CHAINUNLOCK:
	case OP_TDB_CHAINLOCK_READ:
	case OP_TDB_CHAINUNLOCK_READ:
		return NULL;

	case OP_TDB_APPEND:
		if (op->append.pre.dsize == 0)
			return &not_exists_or_empty;
		return &op->append.pre;

	case OP_TDB_STORE:
		if (op->flag == TDB_INSERT) {
			if (op->ret < 0)
				return &must_exist;
			else
				return &must_not_exist;
		} else if (op->flag == TDB_MODIFY) {
			if (op->ret < 0)
				return &must_not_exist;
			else
				return &must_exist;
		}
		/* No flags?  Don't care */
		return NULL;

	case OP_TDB_EXISTS:
		if (op->ret == 1)
			return &must_exist;
		else
			return &must_not_exist;

	case OP_TDB_PARSE_RECORD:
		if (op->ret < 0)
			return &must_not_exist;
		return &must_exist;

	/* FIXME: handle these. */
	case OP_TDB_WIPE_ALL:
	case OP_TDB_FIRSTKEY:
	case OP_TDB_NEXTKEY:
	case OP_TDB_GET_SEQNUM:
	case OP_TDB_TRAVERSE:
	case OP_TDB_TRANSACTION_COMMIT:
	case OP_TDB_TRANSACTION_CANCEL:
	case OP_TDB_TRANSACTION_START:
		return NULL;

	case OP_TDB_FETCH:
		if (!op->data.dptr)
			return &must_not_exist;
		return &op->data;

	case OP_TDB_DELETE:
		if (op->ret < 0)
			return &must_not_exist;
		return &must_exist;

	default:
		errx(1, "Unexpected op type %i", op->type);
	}
	
}

/* What's the data after this op?  pre if nothing changed. */
static const TDB_DATA *gives(const TDB_DATA *key, const TDB_DATA *pre,
			     const struct op *op)
{
	if (starts_transaction(op) || starts_chainlock(op)) {
		unsigned int i;

		/* Cancelled transactions don't change anything. */
		if (op[op->group_len].type == OP_TDB_TRANSACTION_CANCEL)
			return pre;
		assert(op[op->group_len].type == OP_TDB_TRANSACTION_COMMIT
		       || op[op->group_len].type == OP_TDB_CHAINUNLOCK_READ
		       || op[op->group_len].type == OP_TDB_CHAINUNLOCK);

		for (i = 1; i < op->group_len; i++) {
			/* This skips nested transactions, too */
			if (key_eq(op[i].key, *key)
			    || op[i].type == OP_TDB_WIPE_ALL)
				pre = gives(key, pre, &op[i]);
		}
		return pre;
	}

	/* Failed ops don't change state of db. */
	if (op->ret < 0)
		return pre;

	if (op->type == OP_TDB_DELETE || op->type == OP_TDB_WIPE_ALL)
		return &tdb_null;

	if (op->type == OP_TDB_APPEND)
		return &op->append.post;

	if (op->type == OP_TDB_STORE)
		return &op->data;

	return pre;
}

static void add_hash_user(struct keyinfo *hash,
			  unsigned int h,
			  struct op *op[],
			  unsigned int file,
			  unsigned int op_num)
{
	hash[h].user = talloc_realloc(hash, hash[h].user,
				      struct op_desc, hash[h].num_users+1);

	/* If it's in a transaction, it's the transaction which
	 * matters from an analysis POV. */
	if (in_transaction(op[file], op_num)
	    || in_chainlock(op[file], op_num)) {
		unsigned i;

		op_num = op[file][op_num].group_start;

		/* Don't include twice. */
		for (i = 0; i < hash[h].num_users; i++) {
			if (hash[h].user[i].file == file
			    && hash[h].user[i].op_num == op_num)
				return;
		}
	}
	hash[h].user[hash[h].num_users].op_num = op_num;
	hash[h].user[hash[h].num_users].file = file;
	hash[h].num_users++;
}

static struct keyinfo *hash_ops(struct op *op[], unsigned int num_ops[],
				unsigned int num)
{
	unsigned int i, j, h;
	struct keyinfo *hash;

	hash = talloc_zero_array(op[0], struct keyinfo, total_keys*2);
	for (i = 0; i < num; i++) {
		for (j = 1; j < num_ops[i]; j++) {
			/* We can't do this on allocation, due to realloc. */
			list_head_init(&op[i][j].post);
			list_head_init(&op[i][j].pre);

			if (!op[i][j].key.dptr)
				continue;

			h = hash_key(&op[i][j].key) % (total_keys * 2);
			while (!key_eq(hash[h].key, op[i][j].key)) {
				if (!hash[h].key.dptr) {
					hash[h].key = op[i][j].key;
					break;
				}
				h = (h + 1) % (total_keys * 2);
			}
			/* Might as well save some memory if we can. */
			if (op[i][j].key.dptr != hash[h].key.dptr) {
				talloc_free(op[i][j].key.dptr);
				op[i][j].key.dptr = hash[h].key.dptr;
			}

			add_hash_user(hash, h, op, i, j);
		}
	}

	/* Any wipe all entries need adding to all hash entries. */
	for (h = 0; h < total_keys*2; h++) {
		if (!hash[h].num_users)
			continue;

		for (i = 0; i < num_wipe_alls; i++)
			add_hash_user(hash, h, op,
				      wipe_alls[i].file, wipe_alls[i].op_num);
	}

	return hash;
}

static bool satisfies(const TDB_DATA *key, const TDB_DATA *data,
		      const struct op *op)
{
	const TDB_DATA *need = needs(key, op);

	/* Don't need anything?  Cool. */
	if (!need)
		return true;

	/* This should be tdb_null or a real value. */
	assert(data != &must_exist);
	assert(data != &must_not_exist);
	assert(data != &not_exists_or_empty);

	/* Must not exist?  data must not exist. */
	if (need == &must_not_exist)
		return data == &tdb_null;

	/* Must exist? */
	if (need == &must_exist)
		return data != &tdb_null;

	/* Either noexist or empty. */
	if (need == &not_exists_or_empty)
		return data->dsize == 0;

	/* Needs something specific. */
	return key_eq(*data, *need);
}

static void move_to_front(struct op_desc res[], unsigned off, unsigned elem)
{
	if (elem != off) {
		struct op_desc tmp = res[elem];
		memmove(res + off + 1, res + off, (elem - off)*sizeof(res[0]));
		res[off] = tmp;
	}
}

static void restore_to_pos(struct op_desc res[], unsigned off, unsigned elem)
{
	if (elem != off) {
		struct op_desc tmp = res[off];
		memmove(res + off, res + off + 1, (elem - off)*sizeof(res[0]));
		res[elem] = tmp;
	}
}

static bool sort_deps(char *filename[], struct op *op[],
		      struct op_desc res[],
		      unsigned off, unsigned num,
		      const TDB_DATA *key, const TDB_DATA *data,
		      unsigned num_files, unsigned fuzz)
{
	unsigned int i, files_done;
	struct op *this_op;
	bool done[num_files];

	/* None left?  We're sorted. */
	if (off == num)
		return true;

	/* Does this make sequence number go backwards?  Allow a little fuzz. */
	if (off > 0) {
		int seqnum1 = op[res[off-1].file][res[off-1].op_num].seqnum;
		int seqnum2 = op[res[off].file][res[off].op_num].seqnum;

		if (seqnum1 - seqnum2 > (int)fuzz) {
#if DEBUG_DEPS
			printf("Seqnum jump too far (%u -> %u)\n",
			       seqnum1, seqnum2);
#endif
			return false;
		}
	}

	memset(done, 0, sizeof(done));

	/* Since ops within a trace file are ordered, we just need to figure
	 * out which file to try next.  Since we don't take into account
	 * inter-key relationships (which exist by virtue of trace file order),
	 * we minimize the chance of harm by trying to keep in seqnum order. */
	for (files_done = 0, i = off; i < num && files_done < num_files; i++) {
		if (done[res[i].file])
			continue;

		this_op = &op[res[i].file][res[i].op_num];

		/* Is what we have good enough for this op? */
		if (satisfies(key, data, this_op)) {
			move_to_front(res, off, i);
			if (sort_deps(filename, op, res, off+1, num,
				      key, gives(key, data, this_op),
				      num_files, fuzz))
				return true;
			restore_to_pos(res, off, i);
		}
		done[res[i].file] = true;
		files_done++;
	}

	/* No combination worked. */
	return false;
}

static void check_dep_sorting(struct op_desc user[], unsigned num_users,
			      unsigned num_files)
{
#if DEBUG_DEPS
	unsigned int i;
	unsigned minima[num_files];

	memset(minima, 0, sizeof(minima));
	for (i = 0; i < num_users; i++) {
		assert(minima[user[i].file] < user[i].op_num);
		minima[user[i].file] = user[i].op_num;
	}
#endif
}

/* All these ops happen on the same key.  Which comes first?
 *
 * This can happen both because read ops or failed write ops don't
 * change sequence number, and also due to race since we access the
 * number unlocked (the race can cause less detectable ordering problems,
 * in which case we'll deadlock and report: fix manually in that case).
 */
static bool figure_deps(char *filename[], struct op *op[],
			const TDB_DATA *key, const TDB_DATA *data,
			struct op_desc user[],
			unsigned num_users, unsigned num_files)
{
	unsigned int fuzz;

	/* We prefer to keep strict seqnum order if possible: it's the
	 * most likely.  We get more lax if that fails. */
	for (fuzz = 0; fuzz < 100; fuzz = (fuzz + 1)*2) {
		if (sort_deps(filename, op, user, 0, num_users, key, data,
			      num_files, fuzz))
			break;
	}

	if (fuzz >= 100)
		return false;

	check_dep_sorting(user, num_users, num_files);
	return true;
}

/* We're having trouble sorting out dependencies for this key.  Assume that it's
 * a pre-existing record in the db, so determine a likely value. */
static const TDB_DATA *preexisting_data(char *filename[], struct op *op[],
					const TDB_DATA *key,
					struct op_desc *user,
					unsigned int num_users)
{
	unsigned int i;
	const TDB_DATA *data;

	for (i = 0; i < num_users; i++) {
		data = needs(key, &op[user->file][user->op_num]);
		if (data && data != &must_not_exist) {
			if (!quiet)
				printf("%s:%u: needs pre-existing record\n",
				       filename[user->file], user->op_num+1);
			return data;
		}
	}
	return &tdb_null;
}

static void sort_ops(struct tdb_context *tdb,
		     struct keyinfo hash[], char *filename[], struct op *op[],
		     unsigned int num)
{
	unsigned int h;

	/* Gcc nexted function extension.  How cool is this? */
	int compare_seqnum(const void *_a, const void *_b)
	{
		const struct op_desc *a = _a, *b = _b;

		/* First, maintain order within any trace file. */
		if (a->file == b->file)
			return a->op_num - b->op_num;

		/* Otherwise, arrange by seqnum order. */
		if (op[a->file][a->op_num].seqnum !=
		    op[b->file][b->op_num].seqnum)
			return op[a->file][a->op_num].seqnum
				- op[b->file][b->op_num].seqnum;

		/* Cancelled transactions are assumed to happen first. */
		if (starts_transaction(&op[a->file][a->op_num])
		    && !successful_transaction(&op[a->file][a->op_num]))
			return -1;
		if (starts_transaction(&op[b->file][b->op_num])
		    && !successful_transaction(&op[b->file][b->op_num]))
			return 1;

		/* No idea. */
		return 0;
	}

	/* Now sort into seqnum order. */
	for (h = 0; h < total_keys * 2; h++) {
		struct op_desc *user = hash[h].user;

		qsort(user, hash[h].num_users, sizeof(user[0]), compare_seqnum);
		if (!figure_deps(filename, op, &hash[h].key, &tdb_null, user,
				 hash[h].num_users, num)) {
			const TDB_DATA *data;

			data = preexisting_data(filename, op, &hash[h].key,
						user, hash[h].num_users);
			/* Give the first op what it wants: does that help? */
			if (!figure_deps(filename, op, &hash[h].key, data, user,
					 hash[h].num_users, num))
				fail(filename[user[0].file], user[0].op_num+1,
				     "Could not resolve inter-dependencies");
			if (tdb_store(tdb, hash[h].key, *data, TDB_INSERT) != 0)
				errx(1, "Could not store initial value");
		}
	}
}

static int destroy_depend(struct depend *dep)
{
	list_del(&dep->pre_list);
	list_del(&dep->post_list);
	return 0;
}

static void add_dependency(void *ctx,
			   struct op *op[],
			   char *filename[],
			   const struct op_desc *needs,
			   const struct op_desc *prereq)
{
	struct depend *dep;

	/* We don't depend on ourselves. */
	if (needs->file == prereq->file) {
		assert(prereq->op_num < needs->op_num);
		return;
	}

#if DEBUG_DEPS
	printf("%s:%u: depends on %s:%u\n",
	       filename[needs->file], needs->op_num+1,
	       filename[prereq->file], prereq->op_num+1);
#endif

	dep = talloc(ctx, struct depend);
	dep->needs = *needs;
	dep->prereq = *prereq;

#if TRAVERSALS_TAKE_TRANSACTION_LOCK
	/* If something in a traverse depends on something in another
	 * traverse/transaction, it creates a dependency between the
	 * two groups. */
	if ((in_traverse(op[prereq->file], prereq->op_num)
	     && (starts_transaction(&op[needs->file][needs->op_num])
		 || starts_traverse(&op[needs->file][needs->op_num])))
	    || (in_traverse(op[needs->file], needs->op_num)
		&& (starts_transaction(&op[prereq->file][prereq->op_num])
		    || starts_traverse(&op[prereq->file][prereq->op_num])))) {
		unsigned int start;

		/* We are satisfied by end of group. */
		start = op[prereq->file][prereq->op_num].group_start;
		dep->prereq.op_num = start + op[prereq->file][start].group_len;
		/* And we need that done by start of our group. */
		dep->needs.op_num = op[needs->file][needs->op_num].group_start;
	}

	/* There is also this case:
	 *  <traverse> <read foo> ...
	 *  <transaction> ... </transaction> <create foo>
	 * Where if we start the traverse then wait, we could block
	 * the transaction and deadlock.
	 *
	 * We try to address this by ensuring that where seqnum indicates it's
	 * possible, we wait for <create foo> before *starting* traverse.
	 */
	else if (in_traverse(op[needs->file], needs->op_num)) {
		struct op *need = &op[needs->file][needs->op_num];
		if (op[needs->file][need->group_start].seqnum >
		    op[prereq->file][prereq->op_num].seqnum) {
			dep->needs.op_num = need->group_start;
		}
	}
#endif

 	/* If you depend on a transaction or chainlock, you actually
	 * depend on it ending. */
 	if (starts_transaction(&op[prereq->file][dep->prereq.op_num])
	    || starts_chainlock(&op[prereq->file][dep->prereq.op_num])) {
 		dep->prereq.op_num
			+= op[dep->prereq.file][dep->prereq.op_num].group_len;
#if DEBUG_DEPS
		printf("-> Actually end of transaction %s:%u\n",
		       filename[dep->prereq->file], dep->prereq->op_num+1);
#endif
 	} else
		/* We should never create a dependency from middle of
		 * a transaction. */
 		assert(!in_transaction(op[prereq->file], dep->prereq.op_num)
		       || op[prereq->file][dep->prereq.op_num].type
 		       == OP_TDB_TRANSACTION_COMMIT
 		       || op[prereq->file][dep->prereq.op_num].type
 		       == OP_TDB_TRANSACTION_CANCEL);

	list_add(&op[dep->prereq.file][dep->prereq.op_num].post,
		 &dep->post_list);
	list_add(&op[dep->needs.file][dep->needs.op_num].pre,
		 &dep->pre_list);
	talloc_set_destructor(dep, destroy_depend);
}

static bool changes_db(const TDB_DATA *key, const struct op *op)
{
	return gives(key, NULL, op) != NULL;
}

static void depend_on_previous(struct op *op[],
			       char *filename[],
			       unsigned int num,
			       struct op_desc user[],
			       unsigned int i,
			       int prev)
{
	bool deps[num];
	int j;

	if (i == 0)
		return;

	if (prev == i - 1) {
		/* Just depend on previous. */
		add_dependency(NULL, op, filename, &user[i], &user[prev]);
		return;
	}

	/* We have to wait for the readers.  Find last one in *each* file. */
	memset(deps, 0, sizeof(deps));
	deps[user[i].file] = true;
	for (j = i - 1; j > prev; j--) {
		if (!deps[user[j].file]) {
			add_dependency(NULL, op, filename, &user[i], &user[j]);
			deps[user[j].file] = true;
		}
	}
}

/* This is simple, but not complete.  We don't take into account
 * indirect dependencies. */
static void optimize_dependencies(struct op *op[], unsigned int num_ops[],
				  unsigned int num)
{
	unsigned int i, j;

	/* There can only be one real dependency on each file */
	for (i = 0; i < num; i++) {
		for (j = 1; j < num_ops[i]; j++) {
			struct depend *dep, *next;
			struct depend *prev[num];

			memset(prev, 0, sizeof(prev));

			list_for_each_safe(&op[i][j].pre, dep, next, pre_list) {
				if (!prev[dep->prereq.file]) {
					prev[dep->prereq.file] = dep;
					continue;
				}
				if (prev[dep->prereq.file]->prereq.op_num
				    < dep->prereq.op_num) {
					talloc_free(prev[dep->prereq.file]);
					prev[dep->prereq.file] = dep;
				} else
					talloc_free(dep);
			}
		}
	}

	for (i = 0; i < num; i++) {
		int deps[num];

		for (j = 0; j < num; j++)
			deps[j] = -1;

		for (j = 1; j < num_ops[i]; j++) {
			struct depend *dep, *next;

			list_for_each_safe(&op[i][j].pre, dep, next, pre_list) {
				if (deps[dep->prereq.file]
				    >= (int)dep->prereq.op_num)
					talloc_free(dep);
				else
					deps[dep->prereq.file]
						= dep->prereq.op_num;
			}
		}
	}
}

#if TRAVERSALS_TAKE_TRANSACTION_LOCK
/* Force an order among the traversals, so they don't deadlock (as much) */
static void make_traverse_depends(char *filename[],
				  struct op *op[], unsigned int num_ops[],
				  unsigned int num)
{
	unsigned int i, num_traversals = 0;
	int j;
	struct op_desc *desc;

	/* Sort by which one runs first. */
	int compare_traverse_desc(const void *_a, const void *_b)
	{
		const struct op_desc *da = _a, *db = _b;
		const struct op *a = &op[da->file][da->op_num],
			*b = &op[db->file][db->op_num];

		if (a->seqnum != b->seqnum)
			return a->seqnum - b->seqnum;

		/* If they have same seqnum, it means one didn't make any
		 * changes.  Thus sort by end in that case. */
		return a[a->group_len].seqnum - b[b->group_len].seqnum;
	}

	desc = talloc_array(NULL, struct op_desc, 1);

	/* Count them. */
	for (i = 0; i < num; i++) {
		for (j = 1; j < num_ops[i]; j++) {
 			/* Traverse start (ignore those in
			 * transactions; they're already covered by
			 * transaction dependencies). */
			if (starts_traverse(&op[i][j])
			    && !in_transaction(op[i], j)) {
				desc = talloc_realloc(NULL, desc,
						      struct op_desc,
						      num_traversals+1);
				desc[num_traversals].file = i;
				desc[num_traversals].op_num = j;
				num_traversals++;
			}
		}
	}
	qsort(desc, num_traversals, sizeof(desc[0]), compare_traverse_desc);

	for (i = 1; i < num_traversals; i++) {
		const struct op *prev = &op[desc[i-1].file][desc[i-1].op_num];
		const struct op *curr = &op[desc[i].file][desc[i].op_num];

		/* Read traverses don't depend on each other (read lock). */
		if (prev->type == OP_TDB_TRAVERSE_READ_START
		    && curr->type == OP_TDB_TRAVERSE_READ_START)
			continue;

		/* Only make dependency if it's clear. */
		if (compare_traverse_desc(&desc[i], &desc[i-1])) {
			/* i depends on end of traverse i-1. */
			struct op_desc end = desc[i-1];
			end.op_num += prev->group_len;
			add_dependency(NULL, op, filename, &desc[i], &end);
		}
	}
	talloc_free(desc);
}

static void set_nonblock(int fd)
{
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK) != 0)
		err(1, "Setting pipe nonblocking");
}

static bool handle_backoff(struct op *op[], int fd)
{
	struct op_desc desc;
	bool handled = false;

	/* Sloppy coding: we assume PIPEBUF never fills. */
	while (read(fd, &desc, sizeof(desc)) != -1) {
		unsigned int i;
		handled = true;
		for (i = desc.op_num; i > 0; i--) {
			if (op[desc.file][i].type == OP_TDB_TRAVERSE) {
				/* We insert a fake end here. */
				op[desc.file][i].type
					= OP_TDB_TRAVERSE_END_EARLY;
				break;
			} else if (starts_traverse(&op[desc.file][i])) {
				unsigned int start = i;
				struct op tmp = op[desc.file][i];
				/* Move the ops outside traverse. */
				memmove(&op[desc.file][i],
					&op[desc.file][i+1],
					(desc.op_num-i-1) * sizeof(op[0][0]));
				op[desc.file][desc.op_num] = tmp;
				while (op[desc.file][i].group_start == start) {
					op[desc.file][i++].group_start
						= desc.op_num;
				}
				break;
			}
		}
	}
	return handled;
}

#else /* !TRAVERSALS_TAKE_TRANSACTION_LOCK */
static bool handle_backoff(struct op *op[], int fd)
{
	return false;
}
#endif

static void derive_dependencies(struct tdb_context *tdb,
				char *filename[],
				struct op *op[], unsigned int num_ops[],
				unsigned int num)
{
	struct keyinfo *hash;
	unsigned int h, i;

	/* Create hash table for faster key lookup. */
	hash = hash_ops(op, num_ops, num);

	/* Sort them by sequence number. */
	sort_ops(tdb, hash, filename, op, num);

	/* Create dependencies back to the last change, rather than
	 * creating false dependencies by naively making each one
	 * depend on the previous.  This has two purposes: it makes
	 * later optimization simpler, and it also avoids deadlock with
	 * same sequence number ops inside traversals (if one
	 * traversal doesn't write anything, two ops can have the same
	 * sequence number yet we can create a traversal dependency
	 * the other way). */
	for (h = 0; h < total_keys * 2; h++) {
		int prev = -1;

		if (hash[h].num_users < 2)
			continue;

		for (i = 0; i < hash[h].num_users; i++) {
			if (changes_db(&hash[h].key, &op[hash[h].user[i].file]
				       [hash[h].user[i].op_num])) {
				depend_on_previous(op, filename, num,
						   hash[h].user, i, prev);
				prev = i;
			} else if (prev >= 0)
				add_dependency(hash, op, filename,
					       &hash[h].user[i],
					       &hash[h].user[prev]);
		}
	}

#if TRAVERSALS_TAKE_TRANSACTION_LOCK
	make_traverse_depends(filename, op, num_ops, num);
#endif

	optimize_dependencies(op, num_ops, num);
}

static struct timeval run_test(char *argv[],
			       unsigned int num_ops[],
			       unsigned int hashsize[],
			       unsigned int tdb_flags[],
			       unsigned int open_flags[],
			       struct op *op[],
			       int fds[2])
{
	unsigned int i;
	struct timeval start, end, diff;
	bool ok = true;

	for (i = 0; argv[i+2]; i++) {
		struct tdb_context *tdb;
		char c;

		switch (fork()) {
		case -1:
			err(1, "fork failed");
		case 0:
			close(fds[1]);
			tdb = tdb_open(argv[1], hashsize[i],
				       tdb_flags[i], open_flags[i], 0600);
			if (!tdb)
				err(1, "Opening tdb %s", argv[1]);

			/* This catches parent exiting. */
			if (read(fds[0], &c, 1) != 1)
				exit(1);
			run_ops(tdb, pipes[i].fd[0], argv+2, op, i, 1,
				num_ops[i], false);
			check_deps(argv[2+i], op[i], num_ops[i]);
			exit(0);
		default:
			break;
		}
	}

	/* Let everything settle. */
	sleep(1);

	if (!quiet)
		printf("Starting run...");
	fflush(stdout);
	gettimeofday(&start, NULL);
	/* Tell them all to go!  Any write of sufficient length will do. */
	if (write(fds[1], hashsize, i) != i)
		err(1, "Writing to wakeup pipe");

	for (i = 0; argv[i + 2]; i++) {
		int status;
		wait(&status);
		if (!WIFEXITED(status)) {
			warnx("Child died with signal %i", WTERMSIG(status));
			ok = false;
		} else if (WEXITSTATUS(status) != 0)
			/* Assume child spat out error. */
			ok = false;
	}
	if (!ok)
		exit(1);

	gettimeofday(&end, NULL);
	if (!quiet)
		printf("done\n");

	if (end.tv_usec < start.tv_usec) {
		end.tv_usec += 1000000;
		end.tv_sec--;
	}
	diff.tv_sec = end.tv_sec - start.tv_sec;
	diff.tv_usec = end.tv_usec - start.tv_usec;
	return diff;
}

static void init_tdb(struct tdb_context *master_tdb,
		     const char *name, unsigned int hashsize)
{
	TDB_DATA key, data;
	struct tdb_context *tdb;

	tdb = tdb_open(name, hashsize, TDB_CLEAR_IF_FIRST|TDB_NOSYNC,
		       O_CREAT|O_TRUNC|O_RDWR, 0600);
	if (!tdb)
		errx(1, "opening tdb %s", name);

	for (key = tdb_firstkey(master_tdb);
	     key.dptr;
	     key = tdb_nextkey(master_tdb, key)) {
		data = tdb_fetch(master_tdb, key);
		if (tdb_store(tdb, key, data, TDB_INSERT) != 0)
			errx(1, "Failed to store initial key");
	}
	tdb_close(tdb);
}

int main(int argc, char *argv[])
{
	struct timeval diff;
	unsigned int i, num_ops[argc], hashsize[argc], tdb_flags[argc], open_flags[argc];
	struct op *op[argc];
	int fds[2];
	struct tdb_context *master;
	unsigned int runs = 1;

	if (argc < 3)
		errx(1, "Usage: %s [--quiet] [-n <number>] <tdbfile> <tracefile>...", argv[0]);

	if (streq(argv[1], "--quiet")) {
		quiet = true;
		argv++;
		argc--;
	}
	if (streq(argv[1], "-n")) {
		runs = atoi(argv[2]);
		argv += 2;
		argc -= 2;
	}

	pipes = talloc_array(NULL, struct pipe, argc - 1);
	for (i = 0; i < argc - 2; i++) {
		if (!quiet)
			printf("Loading tracefile %s...", argv[2+i]);
		fflush(stdout);
		op[i] = load_tracefile(argv+2, i, &num_ops[i], &hashsize[i],
				       &tdb_flags[i], &open_flags[i]);
		if (pipe(pipes[i].fd) != 0)
			err(1, "creating pipe");
		/* Don't truncate, or clear if first: we do that. */
		open_flags[i] &= ~(O_TRUNC);
		tdb_flags[i] &= ~(TDB_CLEAR_IF_FIRST);
		/* Open NOSYNC, to save time. */
		tdb_flags[i] |= TDB_NOSYNC;
		if (!quiet)
			printf("done\n");
	}

	/* Dependency may figure we need to create seed records. */
	master = tdb_open(NULL, 0, TDB_INTERNAL, O_RDWR, 0);
	if (!quiet) {
		printf("Calculating inter-dependencies...");
		fflush(stdout);
	}
	derive_dependencies(master, argv+2, op, num_ops, i);
	if (!quiet)
		printf("done\n");

	for (i = 0; i < runs; i++) {
		init_tdb(master, argv[1], hashsize[0]);

		/* Don't fork for single arg case: simple debugging. */
		if (argc == 3) {
			struct timeval start, end;
			struct tdb_context *tdb;

			tdb = tdb_open(argv[1], hashsize[0], tdb_flags[0],
				       open_flags[0], 0600);
			if (!quiet) {
				printf("Single threaded run...");
				fflush(stdout);
			}
			gettimeofday(&start, NULL);

			run_ops(tdb, pipes[0].fd[0], argv+2, op, 0, 1,
				num_ops[0], false);
			gettimeofday(&end, NULL);
			if (!quiet)
				printf("done\n");
			tdb_close(tdb);

			check_deps(argv[2], op[0], num_ops[0]);
			if (end.tv_usec < start.tv_usec) {
				end.tv_usec += 1000000;
				end.tv_sec--;
			}
			diff.tv_sec = end.tv_sec - start.tv_sec;
			diff.tv_usec = end.tv_usec - start.tv_usec;
			goto print_time;
		}

		if (pipe(fds) != 0)
			err(1, "creating pipe");

#if TRAVERSALS_TAKE_TRANSACTION_LOCK
		if (pipe(pipes[argc-2].fd) != 0)
			err(1, "creating pipe");
		backoff_fd = pipes[argc-2].fd[1];
		set_nonblock(pipes[argc-2].fd[1]);
		set_nonblock(pipes[argc-2].fd[0]);
#endif

		do {
			diff = run_test(argv, num_ops, hashsize, tdb_flags,
					open_flags, op, fds);
		} while (handle_backoff(op, pipes[argc-2].fd[0]));

	print_time:
		if (!quiet)
			printf("Time replaying: ");
		printf("%lu usec\n", diff.tv_sec * 1000000UL + diff.tv_usec);
	}

	exit(0);
}
