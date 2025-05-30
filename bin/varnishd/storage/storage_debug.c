/*-
 * Copyright 2021,2023 UPLEX - Nils Goroll Systemoptimierung
 * All rights reserved.
 *
 * Author: Nils Goroll <nils.goroll@uplex.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * debug helper storage based on malloc
 */

#include "config.h"

#include "cache/cache_varnishd.h"
#include "cache/cache_obj.h"
#include "common/heritage.h"

#include <stdio.h>
#include <stdlib.h>

#include "storage/storage.h"
#include "storage/storage_simple.h"

#include "vtim.h"
#include "vnum.h"

/*
 * if smd was to have its own configuration, we would have to wrap all function
 * pointers from the actual storage implementation (sma). To avoid these
 * complications, we limit to one smd instance and use statics.
 */
static vtim_dur dopen = 0.0;
static unsigned count = 0;
static ssize_t max_size = 0;

/* returns one byte less than requested */
static int v_matchproto_(objgetspace_f)
smd_lsp_getspace(struct worker *wrk, struct objcore *oc, ssize_t *sz,
    uint8_t **ptr)
{
	AN(sz);
	if (*sz > 2)
		(*sz)--;
	return (SML_methods.objgetspace(wrk, oc, sz, ptr));
}

/*
 * returns max_size at most, then fails
 *
 * relies on the actual storage implementation to not use priv2
 */
static int v_matchproto_(objgetspace_f)
smd_max_getspace(struct worker *wrk, struct objcore *oc, ssize_t *sz,
    uint8_t **ptr)
{
	ssize_t used;
	int r;

	AN(sz);
	used = (ssize_t)oc->stobj->priv2;

	VSLb(wrk->vsl, SLT_Debug, "-sdebug getspace: %zd/%zd", used, max_size);

	if (used >= max_size) {
		VSLb(wrk->vsl, SLT_Storage, "-sdebug: max_size=%zd reached", max_size);
		return (0);
	}

	assert(used < max_size);
	*sz = vmin_t(ssize_t, *sz, max_size - used);

	r = SML_methods.objgetspace(wrk, oc, sz, ptr);
	return (r);
}

static void v_matchproto_(objextend_f)
smd_max_extend(struct worker *wrk, struct objcore *oc, ssize_t l)
{

	assert(l > 0);
	oc->stobj->priv2 += (uint64_t)l;
	VSLb(wrk->vsl, SLT_Debug, "-sdebug extend: %zd/%zd", (ssize_t)oc->stobj->priv2, max_size);
	SML_methods.objextend(wrk, oc, l);
}

#define dur_arg(a, s, d)					\
	(! strncmp((a), (s), strlen(s))				\
	 && (d = VNUM_duration(a + strlen(s))) != nan(""))

static int
bytes_arg(char *a, const char *s, ssize_t *sz)
{
	const char *err;
	uintmax_t bytes;

	AN(sz);
	if (strncmp(a, s, strlen(s)))
		return (0);
	a += strlen(s);
	err = VNUM_2bytes(a, &bytes, 0);
	if (err != NULL)
		ARGV_ERR("%s\n", err);
	assert(bytes <= SSIZE_MAX);
	*sz = (ssize_t)bytes;

	return (1);
}


static void smd_open(struct stevedore *stv)
{
	sma_stevedore.open(stv);
	fprintf(stderr, "-sdebug open delay %fs\n", dopen);
	if (dopen > 0.0)
		VTIM_sleep(dopen);
}

static void v_matchproto_(storage_init_f)
smd_init(struct stevedore *parent, int aac, char * const *aav)
{
	struct obj_methods *methods;
	objgetspace_f *getspace = NULL;
	const char *ident;
	int i, ac = 0;
	size_t nac;
	vtim_dur d, dinit = 0.0;
	char **av;	//lint -e429
	char *a;

	if (count++ > 0)
		ARGV_ERR("Only one -s%s instance supported\n", smd_stevedore.name);

	ident = parent->ident;
	memcpy(parent, &sma_stevedore, sizeof *parent);
	parent->ident = ident;
	parent->name = smd_stevedore.name;

	methods = malloc(sizeof *methods);
	AN(methods);
	memcpy(methods, &SML_methods, sizeof *methods);
	parent->methods = methods;

	assert(aac >= 0);
	nac = aac;
	nac++;
	av = calloc(nac, sizeof *av);
	AN(av);
	for (i = 0; i < aac; i++) {
		a = aav[i];
		if (a != NULL) {
			if (! strcmp(a, "lessspace")) {
				if (getspace != NULL) {
					ARGV_ERR("-s%s conflicting options\n",
					    smd_stevedore.name);
				}
				getspace = smd_lsp_getspace;
				continue;
			}
			if (bytes_arg(a, "maxspace=", &max_size)) {
				if (getspace != NULL) {
					ARGV_ERR("-s%s conflicting options\n",
					    smd_stevedore.name);
				}
				getspace = smd_max_getspace;
				methods->objextend = smd_max_extend;
				continue;
			}
			if (dur_arg(a, "dinit=", d)) {
				dinit = d;
				continue;
			}
			if (dur_arg(a, "dopen=", d)) {
				dopen = d;
				continue;
			}
		}
		av[ac] = a;
		ac++;
	}
	assert(ac >= 0);
	assert(ac < (int)nac);
	AZ(av[ac]);

	if (getspace != NULL)
		methods->objgetspace = getspace;

	sma_stevedore.init(parent, ac, av);
	free(av);
	fprintf(stderr, "-sdebug init delay %fs\n", dinit);
	fprintf(stderr, "-sdebug open delay in init %fs\n", dopen);
	if (dinit > 0.0) {
		VTIM_sleep(dinit);
	}
	parent->open = smd_open;
}

const struct stevedore smd_stevedore = {
	.magic		=	STEVEDORE_MAGIC,
	.name		=	"debug",
	.init		=	smd_init,
	// other callbacks initialized in smd_init()
};
