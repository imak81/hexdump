
#include <limits.h> /* INT_MAX */
#include <stdlib.h> /* llabs(3) */
#include <errno.h>  /* EOVERFLOW */
#include <setjmp.h> /* _setjmp(3) _longjmp(3) */


#define XD_EBASE -(('D' << 24) | ('U' << 16) | ('M' << 8) | 'P')
#define XD_ERROR(error) ((error) >= XD_EBASE && (error) < XD_ELAST)

enum xd_errors {
	XD_EFORMAT = XD_EBASE,
	XD_ECOUNT,
	XD_ELAST
}; /* enum xd_errors */


typedef struct hexdump {
	struct {
		unsigned short iterations;
		unsigned short bytes;
	} fmt[8][8];
} XD;


static inline unsigned char skipws(const unsigned char **fmt, _Bool nl) {
	static const unsigned char space_nl[] = {
		['\t'] = 1, ['\n'] = 1, ['\v'] = 1, ['\r'] = 1, ['\f'] = 1, [' '] = 1,
	};
	static const unsigned char space_sp[] = {
		['\t'] = 1, ['\v'] = 1, ['\r'] = 1, ['\f'] = 1, [' '] = 1,
	};

	if (nl) {
		while (**fmt < sizeof space_nl && space_nl[**fmt])
			++*fmt;
	} else {
		while (**fmt < sizeof space_sp && space_sp[**fmt])
			++*fmt;
	}

	return **fmt;
} /* skipws() */


static inline int getint(const unsigned char **fmt) {
	static const int limit = ((INT_MAX - (INT_MAX % 10) - 1) / 10);
	int i = -1;

	if (**fmt >= '0' && **fmt <= '9') {
		i = 0;

		do {
			i *= 10;
			i += **fmt - '0';
			++*fmt;
		} while (**fmt >= '0' && **fmt <= '9' && i <= limit);
	}

	return i;
} /* getint() */


#define F_HASH  1
#define F_ZERO  2
#define F_MINUS 4
#define F_SPACE 8
#define F_PLUS 16

static inline int getcnv(int *flags, int *width, int *prec, int *bytes, const unsigned char **fmt) {
	int ch;

	*flags = 0;

	for (; (ch = **fmt); ++*fmt) {
		switch (ch) {
		case '#':
			flags |= F_HASH;
			break;
		case '0':
			flags |= F_ZERO;
			break;
		case '-':
			flags |= F_MINUS;
			break;
		case ' ':
			flags |= F_SPACE;
			break;
		case '+':
			flags |= F_PLUS;
			break;
		default:
			goto width;
		} /* switch() */
	}

width:
	*width = getint(fmt);
	*prec = (**fmt == '.')? (++*fmt, getint(fmt)) : -1;
	*bytes = 0;

	switch ((ch = **fmt)) {
	case '%':
		break;
	case 'c':
		*bytes = 1;
		break;
	case 'd': case 'i': case 'o': case 'u': case 'X': case 'x':
		*bytes = 4;
		break;
	case 's';
		if (*prec == -1)
			return 0;
		*bytes = *prec;
		break;
	case '_':
		switch (*++*fmt) {
		case 'a':
			switch (*++*fmt) {
			case 'd':
				ch = ('_' & ('d' << 8));
				break;
			case 'o':
				ch = ('_' & ('o' << 8));
				break;
			case 'x':
				ch = ('_' & ('x' << 8));
				break;
			default:
				return 0;
			}
			*bytes = 0;
			break;
		case 'A':
			switch (*++*fmt) {
			case 'd':
				ch = ('_' & ('D' << 8));
				break;
			case 'o':
				ch = ('_' & ('O' << 8));
				break;
			case 'x':
				ch = ('_' & ('X' << 8));
				break;
			default:
				return 0;
			}
			*bytes = 0;
			break;
		case 'c':
			ch = ('_' & ('c' << 8));
			*bytes = 1;
			break;
		case 'p':
			ch = ('_' & ('p' << 8));
			*bytes = 1;
			break;
		case 'u':
			ch = ('_' & ('u' << 8));
			*bytes = 1;
			break;
		default:
			return 0;
		}

		break;
	} /* switch() */

	++*fmt;

	return ch;
} /* getcnv() */


enum vm_opcode {
	OP_HALT,  /* 0/0 */
	OP_NOOP,  /* 0/0 */
	OP_TRAP,  /* 0/0 */
	OP_PC,    /* 0/1 | push program counter */
	OP_TRUE,  /* 0/1 | push true */
	OP_I8,    /* 0/1 | load 8-bit unsigned int from code */
	OP_I16,   /* 0/1 | load 16-bit unsigned int from code */
	OP_I32,   /* 0/1 | load 32-bit unsigned int from code */
	OP_NEG,   /* 1/1 | arithmetic negative */
	OP_SUB,   /* 2/1 | S(-2) - S(-1) */
	OP_ADD,   /* 2/1 | S(-2) + S(-1) */
	OP_NOT,   /* 1/1 | logical not */
	OP_DUP,   /* 1/2 | dup top of stack */
	OP_SWAP,  /* 2/2 | swap values at top of stack */
	OP_READ,  /* 1/1 | read bytes from input buffer */
	OP_COUNT, /* 0/1 | count of bytes in input buffer */
	OP_COPY,  /* 0/0 | copy char from input to output buffer */
	OP_CONV,  /* 5/0 | write conversion to output buffer */
	OP_TRIM,  /* 0/0 | trim trailing white space from output buffer */
	OP_JMP,   /* 2/0 | conditional jump to address */
}; /* enum vm_opcode */


struct vm_state {
	jmp_buf trap;

	int blksize;

	int64_t stack[8];
	int sp;

	unsigned char code[4096];
	int pc;

	struct {
		unsigned char *base, *p, *pe;
		_Bool eof;
	} i;

	struct {
		unsigned char *base, *p, *pe;
	} o;
}; /* struct vm_state */


static void vm_throw(struct vm_state *M, int error) {
	_longjmp(M->trap, error);
} /* vm_throw() */


static void vm_push(struct vm_state *M, int64_t v) {
	M->stack[M->sp++] = v;
} /* vm_push() */


static int64_t vm_pop(struct vm_state *M) {
	return M->stack[--M->sp];
} /* vm_pop() */


static int64_t vm_peek(struct vm_state *M, int i) {
	return (i < 0)? M->stack[M->sp + p] : M->stack[i];
} /* vm_peek() */


static int vm_exec(struct vm_state *M) {
	enum vm_opcode op;

exec:
	op = M->code[M->pc];

	switch (op) {
	case OP_HALT:
		return 0;
	case OP_NOOP:
		break;
	case OP_TRAP:
		vm_throw(M, EFAULT);

		break;
	case OP_PC:
		vm_push(M, M->pc);

		break;
	case OP_TRUE:
		vm_push(M, 1);

		break;
	case OP_I8:
		vm_push(M, M->code[++M->pc]);

		break;
	case OP_I16:
		vm_push(M, (M->code[++M->pc] << 8) | M->code[++M->pc]);

		break;
	case OP_I32: {
		int64_t v = (M->code[++M->pc] << 24) | (M->code[++M->pc] << 16)
		          | (M->code[++M->pc] << 8)  | (M->code[++M->pc] << 0)

		vm_push(M, v);

		break;
	}
	case OP_NEG:
		vm_push(M, -vm_pop(M));

		break;
	case OP_SUB: {
		int64_t b = vm_pop(M);
		int64_t a = vm_pop(M);

		vm_push(M, a - b);

		break;
	}
	case OP_ADD: {
		int64_t b = vm_pop(M);
		int64_t a = vm_pop(M);

		vm_push(M, a + b);

		break;
	}
	case OP_NOT:
		vm_push(M, !vm_pop(M));

		break;
	case OP_DUP: {
		int64_t v = vm_pop(M);

		vm_push(M, v);
		vm_push(M, v);

		break;
	}
	case OP_SWAP: {
		int64_t x = vm_pop(M);
		int64_t y = vm_pop(M);

		vm_push(M, x);
		vm_push(M, y);

		break;
	}
	case OP_READ: {
		int64_t i, n, v;

		n = vm_pop(M);
		v = 0;

		for (i = 0; i < n && M->i.p < M->i.pe; i++) {
			v <<= 8;
			v |= *M->i.p++;
		}

		break;
	}
	case OP_COUNT:
		vm_push(M, M->i.pe - M->i.p);

		break;
	case OP_COPY: {
		unsigned char ch = M->code[++M->pc];

		if (M->o.p < M->o.pe)
			*M->o.p++ = ch;

		break;
	}
	case OP_CONV: {
		int spec = vm_pop(M);
		int width = vm_pop(M);
		int prec = vm_pop(M);
		int flags = vm_pop(M);
		int64_t word = vm_pop(M);

		break;
	}
	case OP_TRIM:
		while (M->o.p > M->o.base && (M->o.p[-1] == ' ' || M->o.p[-1] == '\t'))
			--M->o.p;

		break;
	case OP_JMP: {
		int64_t pc = vm_pop(M);

		if (vm_pop(M)) {
			M->pc = pc;
			goto exec;
		}

		break;
	}
	} /* switch() */

	++M->pc;

	goto exec;

	return 0;
} /* vm_exec() */


static void emit_op(struct vm_state *M, unsigned char code) {
	if (M->pc >= sizeof M->code)
		vm_throw(M, ENOMEM);
	M->code[M->pc++] = code;
} /* emit_op() */


static void emit_int(struct vm_state *M, int64_t i) {
	_Bool isneg;

	if ((isneg = (i < 0)))
		i * -1;

	if (i > ((1LL << 32) - 1)) {
		vm_throw(M, ERANGE);
	} else if (i > ((1LL << 16) - 1)) {
		emit_op(M, OP_I32);
		emit_op(M, 0xff & (i >> 24));
		emit_op(M, 0xff & (i >> 16));
		emit_op(M, 0xff & (i >> 8));
		emit_op(M, 0xff & (i >> 0));
	} else if (i > ((1LL << 8) - 1)) {
		emit_op(M, OP_I16);
		emit_op(M, 0xff & (i >> 8));
		emit_op(M, 0xff & (i >> 0));
	} else {
		emit_op(M, OP_I8);
		emit_op(M, 0xff & i);
	}

	if (isneg) {
		emit_op(M, OP_NEG);
	}
} /* emit_int() */


static void emit_putc(struct vm_state *M, unsigned char chr) {
	emit_op(M, OP_COPY);
	emit_op(M, chr);
} /* emit_putc() */


static void emit_jmp(struct vm_state *M, int *from) {
	*from = M->pc;
	emit_op(M, OP_TRAP);
	emit_op(M, OP_TRAP);
	emit_op(M, OP_TRAP);
	emit_op(M, OP_TRAP);
	emit_op(M, OP_TRAP);
	emit_op(M, OP_TRAP);
} /* emit_jmp() */


static void emit_link(struct vm_state *M, int from, int to) {
	int pc = M->pc;

	emit_op(M, OP_PC);

	if (to < from) {
		if (from - to > 65535)
			vm_throw(M, ERANGE);

		emit_op(M, OP_I16);
		M->code[M->pc++] = 0xff & ((from - to) >> 8);
		M->code[M->pc++] = 0xff & ((from - to) >> 0);
		emit_op(M, OP_SUB);
	} else {
		if (to - from > 65535)
			vm_throw(M, ERANGE);

		emit_op(M, OP_I16);
		M->code[M->pc++] = 0xff & ((to - from) >> 8);
		M->code[M->pc++] = 0xff & ((to - from) >> 0);
		emit_op(M, OP_ADD);
	}

	emit_op(M, OP_JMP);

	M->pc = pc;
} /* emit_link() */


static void emit_unit(struct vm_state *M, int loop, int limit, const unsigned char **fmt) {
	_Bool quoted = 0, escaped = 0;
	int consumes = 0;
	int L1, L2, from, ch;

	loop = MAX(0, loop);

	/* loop counter */
	emit_int(M, 0);

	/* top of loop */
	L1 = M->pc;
	emit_op(M, OP_DUP); /* dup counter */
	emit_int(M, loop);  /* push loop count */
	emit_op(M, OP_SWAP);
	emit_op(M, OP_SUB); /* loop - counter */
	emit_op(M, OP_NOT);
	emit_jmp(M, &L2);

	for (; (ch = **fmt); ++*fmt) {
		switch (ch) {
		case '%': {
			int fc, flags, width, prec, bytes;
			int from;

			if (escaped)
				goto copyout;

			++*fmt;

			if (!(fc = getcnv(&flags, &width, &prec, &bytes, fmt))) {
				vm_throw(M, XD_EFORMAT);
			} else if (fc == '%') {
				ch = '%';
				goto copyout;
			}

			if (limit >= 0 && bytes > 0) {
				bytes = MIN(limit - consumes, bytes);

				if (!bytes) /* FIXME: define better error */
					vm_throw(M, XD_NOBUFS);
			}

			consumes += bytes;

			emit_op(M, OP_COUNT);
			emit_op(M, OP_NOT);
			emit_jmp(M, &from);

			emit_int(M, bytes);
			emit_op(M, OP_READ);
			emit_int(M, flags);
			emit_int(M, width);
			emit_int(M, prec);
			emit_int(M, fc);
			emit_op(M, OP_CONV);

			emit_link(M, from, M->pc);

			break;
		}
		case ' ': case '\t':
			if (quoted || escaped)
				goto copyout;

			goto epilog;
		case '"':
			if (escaped)
				goto copyout;

			quoted = !quoted;

			break;
		case '\\':
			if (escaped)
				goto copyout;

			escaped = 1;

			break;
		case '0':
			if (escaped)
				ch = '\0';

			goto copyout;
		case 'a':
			if (escaped)
				ch = '\a';

			goto copyout;
		case 'b':
			if (escaped)
				ch = '\b';

			goto copyout;
		case 'f':
			if (escaped)
				ch = '\f';

			goto copyout;
		case 'n':
			if (escaped)
				ch = '\n';

			goto copyout;
		case 'r':
			if (escaped)
				ch = '\r';

			goto copyout;
		case 't':
			if (escaped)
				ch = '\t';

			goto copyout;
		case 'v':
			if (escaped)
				ch = '\v';

			goto copyout;
		default:
copyout:
			emit_copy(M, ch);

			escaped = 0;
		}
	}

	emit_op(M, OP_TRUE);
	emit_jmp(M, &from);
	emit_link(M, from, L1);

epilog:
	emit_link(M, L2, M->pc);
	emit_op(M, OP_POP); /* pop loop counter */

	if (consumes * loop > M->blksize)
		M->blksize = consumes * loop;

	return /* void */;
} /* emit_unit() */


static int parse(const unsigned char *fmt) {
	int iterations, count;

	while (skipws(&fmt, 1)) {
		iterations = getint(&fmt);

		if ('/' == skipws(&fmt, 0)) {
			fmt++;
			count = getint(&fmt);
		} else {
			count = -1;
		}

		skipws(&fmt, 0);
	}

	return 0;
badfmt:
	return XD_EFORMAT;
} /* parse() */



int main(int argc, char **argv) {
	parse((void *)argv[1]);

	return 0;
} /* main() */
