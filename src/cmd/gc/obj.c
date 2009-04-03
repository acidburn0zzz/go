// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "go.h"

/*
 * architecture-independent object file output
 */

void
dumpobj(void)
{
	bout = Bopen(outfile, OWRITE);
	if(bout == nil)
		fatal("cant open %s", outfile);

	Bprint(bout, "%s\n", thestring);
	Bprint(bout, "  exports automatically generated from\n");
	Bprint(bout, "  %s in package \"%s\"\n", curio.infile, package);
	dumpexport();
	Bprint(bout, "\n!\n");

	outhist(bout);

	// add nil plist w AEND to catch
	// auto-generated trampolines, data
	newplist();

	dumpglobls();
	dumpstrings();
	dumpsignatures();
	dumpfuncs();

	Bterm(bout);
}

void
dumpglobls(void)
{
	Dcl *d;
	Sym *s;
	Node *n;

	// add globals
	for(d=externdcl; d!=D; d=d->forw) {
		if(d->op != ONAME)
			continue;

		s = d->dsym;
		if(s == S)
			fatal("external nil");
		n = d->dnode;
		if(n == N || n->type == T)
			fatal("external %S nil\n", s);

		if(n->class == PFUNC)
			continue;

		dowidth(n->type);
		ggloblnod(s->oname, n->type->width);
	}
}

void
Bputdot(Biobuf *b)
{
	// put out middle dot ·
	Bputc(b, 0xc2);
	Bputc(b, 0xb7);
}

void
outhist(Biobuf *b)
{
	Hist *h;
	char *p, *q, *op;
	int n;

	for(h = hist; h != H; h = h->link) {
		p = h->name;
		op = 0;

		if(p && p[0] != '/' && h->offset == 0 && pathname && pathname[0] == '/') {
			op = p;
			p = pathname;
		}

		while(p) {
			q = utfrune(p, '/');
			if(q) {
				n = q-p;
				if(n == 0)
					n = 1;		// leading "/"
				q++;
			} else {
				n = strlen(p);
				q = 0;
			}
			if(n)
				zfile(b, p, n);
			p = q;
			if(p == 0 && op) {
				p = op;
				op = 0;
			}
		}

		zhist(b, h->line, h->offset);
	}
}

void
ieeedtod(uint64 *ieee, double native)
{
	double fr, ho, f;
	int exp;
	uint32 h, l;

	if(native < 0) {
		ieeedtod(ieee, -native);
		*ieee |= 1ULL<<63;
		return;
	}
	if(native == 0) {
		*ieee = 0;
		return;
	}
	fr = frexp(native, &exp);
	f = 2097152L;		/* shouldnt use fp constants here */
	fr = modf(fr*f, &ho);
	h = ho;
	h &= 0xfffffL;
	h |= (exp+1022L) << 20;
	f = 65536L;
	fr = modf(fr*f, &ho);
	l = ho;
	l <<= 16;
	l |= (int32)(fr*f);
	*ieee = ((uint64)h << 32) | l;
}

/*
 * Add DATA for signature s.
 *	progt - type in program
 *	ifacet - type stored in interface (==progt if small, ==ptrto(progt) if large)
 *	rcvrt - type used as method interface.  eqtype(ifacet, rcvrt) is always true,
 *		but ifacet might have a name that rcvrt does not.
 *	methodt - type with methods hanging off it (progt==*methodt sometimes)
 *
 * memory layout is Sigt struct from iface.c:
 *	struct	Sigt
 *	{
 *		byte*	name;                   // name of basic type
 *		Sigt*	link;			// for linking into hash tables
 *		uint32	thash;                  // hash of type
 *		uint32	mhash;                  // hash of methods
 *		uint16	width;			// width of base type in bytes
 *		uint16	alg;			// algorithm
 *		struct {
 *			byte*	fname;
 *			uint32	fhash;		// hash of type
 *			uint32	offset;		// offset of substruct
 *			void	(*fun)(void);
 *		} meth[1];			// one or more - last name is nil
 *	};
 */

static int
sigcmp(Sig *a, Sig *b)
{
	return strcmp(a->name, b->name);
}

void
dumpsigt(Type *progt, Type *ifacet, Type *rcvrt, Type *methodt, Sym *s)
{
	Type *f;
	int o;
	Sig *a, *b;
	char buf[NSYMB];
	Type *this;
	Iter savet;
	Prog *oldlist;
	Sym *method;
	uint32 sighash;
	int ot;

	a = nil;
	o = 0;
	oldlist = nil;
	sighash = typehash(progt, 1, 0);
	for(f=methodt->method; f!=T; f=f->down) {
		if(f->type->etype != TFUNC)
			continue;

		if(f->etype != TFIELD)
			fatal("dumpsignatures: not field");

		method = f->sym;
		if(method == nil)
			continue;

		b = mal(sizeof(*b));
		b->link = a;
		a = b;

		a->name = method->name;
		a->hash = PRIME8*stringhash(a->name) + PRIME9*typehash(f->type, 0, 0);
		if(!exportname(a->name))
			a->hash += PRIME10*stringhash(package);
		a->perm = o;
		a->sym = methodsym(method, rcvrt);

		sighash = sighash*100003 + a->hash;

		if(!a->sym->siggen) {
			a->sym->siggen = 1;
			// TODO(rsc): This test is still not quite right.

			this = structfirst(&savet, getthis(f->type))->type;
			if(isptr[this->etype] != isptr[ifacet->etype]) {
				if(oldlist == nil)
					oldlist = pc;

				// indirect vs direct mismatch
				Sym *oldname, *newname;
				Type *oldthis, *newthis;

				newthis = ifacet;
				if(isptr[newthis->etype])
					oldthis = ifacet->type;
				else
					oldthis = ptrto(ifacet);
				newname = a->sym;
				oldname = methodsym(method, oldthis);
				genptrtramp(method, oldname, oldthis, f->type, newname, newthis);
			} else
			if(f->embedded) {
				// TODO(rsc): only works for pointer receivers
				if(oldlist == nil)
					oldlist = pc;
				genembedtramp(ifacet, a);
			}
		}
		o++;
	}

	// restore data output
	if(oldlist) {
		// old list ended with AEND; change to ANOP
		// so that the trampolines that follow can be found.
		nopout(oldlist);

		// start new data list
		newplist();
	}

	a = lsort(a, sigcmp);
	ot = 0;
	ot = rnd(ot, maxround);	// base structure

	// base of type signature contains parameters
	snprint(buf, sizeof buf, "%#T", progt);
	ot = dstringptr(s, ot, buf);		// name
	ot = duintptr(s, ot, 0);	// skip link
	ot = duint32(s, ot, typehash(progt, 1, 0));	// thash
	ot = duint32(s, ot, sighash);			// mhash
	ot = duint16(s, ot, progt->width);		// width
	ot = duint16(s, ot, algtype(progt));		// algorithm

	for(b=a; b!=nil; b=b->link) {
		ot = rnd(ot, maxround);		// base of substructure
		ot = dstringptr(s, ot, b->name);	// field name
		ot = duint32(s, ot, b->hash);		// hash
		ot = duint32(s, ot, 0);		// offset
		ot = dsymptr(s, ot, b->sym);		// &method
	}

	// nil field name at end
	ot = rnd(ot, maxround);
	ot = duintptr(s, ot, 0);

	// set DUPOK to allow other .6s to contain
	// the same signature.  only one will be chosen.
	// should only happen for empty signatures
	ggloblsym(s, ot, a == nil);
}

/*
 * memory layout is Sigi struct from iface.c:
 *	struct	Sigi
 *	{
 *		byte*	name;
 *		uint32	hash;
 *		uint32	size;			// number of methods
 *		struct {
 *			byte*	fname;
 *			uint32	fhash;
 *			uint32	perm;		// location of fun in Sigt
 *		} meth[1];			// [size+1] - last name is nil
 *	};
 */
void
dumpsigi(Type *t, Sym *s)
{
	Type *f;
	Sym *s1;
	int o;
	Sig *a, *b;
	char buf[NSYMB];
	uint32 sighash;
	int ot;

	a = nil;
	o = 0;
	sighash = 0;
	for(f=t->type; f!=T; f=f->down) {
		if(f->type->etype != TFUNC)
			continue;

		if(f->etype != TFIELD)
			fatal("dumpsignatures: not field");

		s1 = f->sym;
		if(s1 == nil)
			continue;

		b = mal(sizeof(*b));
		b->link = a;
		a = b;

		a->name = s1->name;
		a->hash = PRIME8*stringhash(a->name) + PRIME9*typehash(f->type, 0, 0);
		if(!exportname(a->name))
			a->hash += PRIME10*stringhash(package);
		a->perm = o;
		a->sym = methodsym(f->sym, t);
		a->offset = 0;

		sighash = sighash*100003 + a->hash;

		o++;
	}

	a = lsort(a, sigcmp);
	ot = 0;
	ot = rnd(ot, maxround);	// base structure

	// sigi[0].name = type name, for runtime error message
	snprint(buf, sizeof buf, "%#T", t);
	ot = dstringptr(s, ot, buf);

	// first field of an interface signature
	// contains the count and is not a real entry

	// sigi[0].hash = sighash
	ot = duint32(s, ot, sighash);

	// sigi[0].offset = count
	o = 0;
	for(b=a; b!=nil; b=b->link)
		o++;
	ot = duint32(s, ot, o);

	for(b=a; b!=nil; b=b->link) {
//print("	%s\n", b->name);
		ot = rnd(ot, maxround);	// base structure

		// sigx[++].name = "fieldname"
		// sigx[++].hash = hashcode
		// sigi[++].perm = mapped offset of method
		ot = dstringptr(s, ot, b->name);
		ot = duint32(s, ot, b->hash);
		ot = duint32(s, ot, b->perm);
	}

	// nil field name at end
	ot = rnd(ot, maxround);
	ot = duintptr(s, ot, 0);

	// TODO(rsc): DUPOK should not be necessary here,
	// and I am a bit worried that it is.  If I turn it off,
	// I get multiple definitions for sigi.dotdotdot.
	ggloblsym(s, ot, 1);
}

void
dumpsignatures(void)
{
	int et;
	Dcl *d, *x;
	Type *t, *progt, *methodt, *ifacet, *rcvrt;
	Sym *s;

	// copy externdcl list to signatlist
	for(d=externdcl; d!=D; d=d->forw) {
		if(d->op != OTYPE)
			continue;

		t = d->dtype;
		if(t == T)
			continue;

		s = signame(t);
		if(s == S)
			continue;

		x = mal(sizeof(*d));
		x->op = OTYPE;
		if(t->etype == TINTER)
			x->dtype = t;
		else
			x->dtype = ptrto(t);
		x->forw = signatlist;
		x->block = 0;
		signatlist = x;
//print("SIG = %lS %lS %lT\n", d->dsym, s, t);
	}

	// process signatlist
	for(d=signatlist; d!=D; d=d->forw) {
		if(d->op != OTYPE)
			continue;
		t = d->dtype;
		et = t->etype;
		s = signame(t);
//print("signame %S for %T\n", s, t);
		if(s == S)
			continue;

		// only emit one
		if(s->siggen)
			continue;
		s->siggen = 1;

		// interface is easy
		if(et == TINTER || et == TDDD) {
			if(t->sym && !t->local)
				continue;
			dumpsigi(t, s);
			continue;
		}

		// non-interface is more complex
		progt = t;
		methodt = t;
		ifacet = t;
		rcvrt = t;

		// if there's a pointer, methods are on base.
		if(isptr[methodt->etype] && methodt->type->sym != S) {
			methodt = methodt->type;
			expandmeth(methodt->sym, methodt);

			// if methodt had a name, we don't want to see
			// it in the method names that go into the sigt.
			// e.g., if
			//	type item *rat
			// then item needs its own sigt distinct from *rat,
			// but it needs to have all of *rat's methods, using
			// the *rat (not item) in the method names.
			if(rcvrt->sym != S)
				rcvrt = ptrto(methodt);
		}

		// and if ifacet is too wide, the methods
		// will see a pointer anyway.
		if(ifacet->width > 8) {
			ifacet = ptrto(progt);
			rcvrt = ptrto(progt);
		}

		// don't emit non-trivial signatures for types defined outside this file.
		// non-trivial signatures might also drag in generated trampolines,
		// and ar can't handle duplicates of the trampolines.
		// only pay attention to types with symbols, because
		// the ... structs and maybe other internal structs
		// don't get marked as local.
		if(methodt->method && methodt->sym && !methodt->local)
			continue;

//print("s=%S\n", s);
		dumpsigt(progt, ifacet, rcvrt, methodt, s);
	}

	if(stringo > 0)
		ggloblsym(symstringo, stringo, 0);
}

void
stringpool(Node *n)
{
	Pool *p;
	int w;

	if(n->op != OLITERAL || n->val.ctype != CTSTR) {
		if(n->val.ctype == CTNIL)
			return;
		fatal("stringpool: not string %N", n);
	}

	p = mal(sizeof(*p));

	p->sval = n->val.u.sval;
	p->link = nil;

	if(poolist == nil)
		poolist = p;
	else
		poolast->link = p;
	poolast = p;

	w = types[TINT32]->width;
	symstringo->offset += w;		// len
	symstringo->offset += p->sval->len;	// str[len]
	symstringo->offset = rnd(symstringo->offset, w);
}

Sig*
lsort(Sig *l, int(*f)(Sig*, Sig*))
{
	Sig *l1, *l2, *le;

	if(l == 0 || l->link == 0)
		return l;

	l1 = l;
	l2 = l;
	for(;;) {
		l2 = l2->link;
		if(l2 == 0)
			break;
		l2 = l2->link;
		if(l2 == 0)
			break;
		l1 = l1->link;
	}

	l2 = l1->link;
	l1->link = 0;
	l1 = lsort(l, f);
	l2 = lsort(l2, f);

	/* set up lead element */
	if((*f)(l1, l2) < 0) {
		l = l1;
		l1 = l1->link;
	} else {
		l = l2;
		l2 = l2->link;
	}
	le = l;

	for(;;) {
		if(l1 == 0) {
			while(l2) {
				le->link = l2;
				le = l2;
				l2 = l2->link;
			}
			le->link = 0;
			break;
		}
		if(l2 == 0) {
			while(l1) {
				le->link = l1;
				le = l1;
				l1 = l1->link;
			}
			break;
		}
		if((*f)(l1, l2) < 0) {
			le->link = l1;
			le = l1;
			l1 = l1->link;
		} else {
			le->link = l2;
			le = l2;
			l2 = l2->link;
		}
	}
	le->link = 0;
	return l;
}

