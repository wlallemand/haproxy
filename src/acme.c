/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Implements the ACMEv2 RFC 8555 protocol
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <import/ebsttree.h>
#include <import/mjson.h>

#include <haproxy/acme-t.h>

#include <haproxy/cli.h>
#include <haproxy/cfgparse.h>
#include <haproxy/errors.h>
#include <haproxy/jws.h>

#include <haproxy/base64.h>
#include <haproxy/cfgparse.h>
#include <haproxy/cli.h>
#include <haproxy/errors.h>
#include <haproxy/http_client.h>
#include <haproxy/jws.h>
#include <haproxy/list.h>
#include <haproxy/ssl_ckch.h>
#include <haproxy/ssl_sock.h>
#include <haproxy/ssl_utils.h>
#include <haproxy/tools.h>

static struct acme_cfg *acme_cfgs = NULL;
static struct acme_cfg *cur_acme = NULL;

/* Return an existing acme_cfg section */
struct acme_cfg *get_acme_cfg(const char *name)
{
	struct acme_cfg *tmp_acme = acme_cfgs;

	/* first check if the ID was already used */
	while (tmp_acme) {
		if (strcmp(tmp_acme->name, name) == 0)
			return tmp_acme;

		tmp_acme = tmp_acme->next;
	}
	return NULL;
}

/* Return an existing section section OR create one and return it */
struct acme_cfg *new_acme_cfg(const char *name)
{
	struct acme_cfg *ret = NULL;

	/* first check if the ID was already used. return it if that's the case */
	if ((ret = get_acme_cfg(name)) != NULL)
		goto out;

	/* If there wasn't any section with this name, just create one */
	ret = calloc(1, sizeof(*ret));
	if (!ret)
		return NULL;

	ret->name = strdup(name);
	/* 0 on the linenum just mean it was not initialized yet */
	ret->linenum = 0;

	ret->challenge = strdup("HTTP-01"); /* default value */

	/* The default generated keys are EC-384 */
	ret->key.type = EVP_PKEY_EC;
	ret->key.curves = NID_secp384r1;

	/* default to 4096 bits when using RSA */
	ret->key.bits = 4096;

	ret->next = acme_cfgs;
	acme_cfgs = ret;

out:
	return ret;
}

/*
 * ckch_conf acme parser
 */
int ckch_conf_acme_init(void *value, char *buf, struct ckch_data *d, int cli, const char *filename, int linenum, char **err)
{
	int err_code = 0;
	struct acme_cfg *cfg;

	cfg = new_acme_cfg(value);
	if (!cfg) {
		memprintf(err, "out of memory.\n");
		err_code |= ERR_FATAL| ERR_ALERT;
		goto error;
	}

	if (cfg->linenum == 0) {
		if (filename)
			cfg->filename = strdup(filename);
                /* store the linenum as a negative value because is the one of
                 * the crt-store, not the one of the section. It will be replace
                 * by the one of the section once initialized
                 */
                cfg->linenum = -linenum;
	}

error:
	return err_code;
}


/* acme section parser
 * Fill the acme_cfgs linked list
 */
static int cfg_parse_acme(const char *file, int linenum, char **args, int kwm)
{
	struct cfg_kw_list *kwl;
	const char *best;
	int index;
	int rc = 0;
	int err_code = 0;
	char *errmsg = NULL;

	if (!experimental_directives_allowed) {
		ha_alert("parsing [%s:%d]: section '%s' is experimental, must be allowed via a global 'expose-experimental-directives'\n", file, linenum, cursection);
		err_code |= ERR_ALERT | ERR_FATAL;
		goto out;
	}

	if (strcmp(args[0], "acme") == 0) {
		struct acme_cfg *tmp_acme = acme_cfgs;

		if (alertif_too_many_args(1, file, linenum, args, &err_code))
			goto out;

		if (!*args[1]) {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: section '%s' requires an ID argument.\n", file, linenum, cursection);
			goto out;
		}

		cur_acme = new_acme_cfg(args[1]);
		if (!cur_acme) {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: out of memory.\n", file, linenum);
			goto out;
		}


		/* first check if the ID was already used */
		if (cur_acme->linenum > 0) {
			/* an unitialized section is created when parsing the "acme" keyword in a crt-store, with a
			 * linenum <= 0, however, when the linenum > 0, it means we already created a section with this
			 * name */
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: acme section '%s' already exists (%s:%d).\n",
					file, linenum, args[1], tmp_acme->filename, tmp_acme->linenum);
			goto out;
		}

		cur_acme->filename = (char *)file;
		cur_acme->linenum = linenum;

		goto out;
	}

	list_for_each_entry(kwl, &cfg_keywords.list, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			if (kwl->kw[index].section != CFG_ACME)
				continue;
			if (strcmp(kwl->kw[index].kw, args[0]) == 0) {
				if (check_kw_experimental(&kwl->kw[index], file, linenum, &errmsg)) {
					ha_alert("%s\n", errmsg);
					err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
					goto out;
				}

				/* prepare error message just in case */
				rc = kwl->kw[index].parse(args, CFG_ACME, NULL, NULL, file, linenum, &errmsg);
				if (rc & ERR_ALERT) {
					ha_alert("parsing [%s:%d] : %s\n", file, linenum, errmsg);
					err_code |= rc;
					goto out;
				}
				else if (rc & ERR_WARN) {
					ha_warning("parsing [%s:%d] : %s\n", file, linenum, errmsg);
					err_code |= rc;
					goto out;
				}
				goto out;
			}
		}
	}

	best = cfg_find_best_match(args[0], &cfg_keywords.list, CFG_ACME, NULL);
	if (best)
		ha_alert("parsing [%s:%d] : unknown keyword '%s' in '%s' section; did you mean '%s' maybe ?\n", file, linenum, args[0], cursection, best);
	else
		ha_alert("parsing [%s:%d] : unknown keyword '%s' in '%s' section\n", file, linenum, args[0], cursection);
	err_code |= ERR_ALERT | ERR_FATAL;
	goto out;

out:
	if (err_code & ERR_FATAL)
		err_code |= ERR_ABORT;
	free(errmsg);
	return err_code;


}

static int cfg_parse_acme_kws(char **args, int section_type, struct proxy *curpx, const struct proxy *defpx,
                              const char *file, int linenum, char **err)
{
	int err_code = 0;
	char *errmsg = NULL;

	if (strcmp(args[0], "uri") == 0) {
		if (!*args[1]) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires an argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
		if (alertif_too_many_args(1, file, linenum, args, &err_code))
			goto out;
		cur_acme->uri = strdup(args[1]);
		if (!cur_acme->uri) {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: out of memory.\n", file, linenum);
			goto out;
		}
	} else if (strcmp(args[0], "contact") == 0) {
		/* save the contact email */
		if (!*args[1]) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires an argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
		if (alertif_too_many_args(1, file, linenum, args, &err_code))
			goto out;

		cur_acme->account.contact = strdup(args[1]);
		if (!cur_acme->account.contact) {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: out of memory.\n", file, linenum);
			goto out;
		}
	} else if (strcmp(args[0], "account") == 0) {
		/* save the filename of the account key */
		if (!*args[1]) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires a filename argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
		if (alertif_too_many_args(2, file, linenum, args, &err_code))
			goto out;

		cur_acme->account.file = strdup(args[1]);
		if (!cur_acme->account.file) {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: out of memory.\n", file, linenum);
			goto out;
		}
	} else if (strcmp(args[0], "challenge") == 0) {
		if ((!*args[1]) ||  (strcmp("HTTP-01", args[1]) != 0 && (strcmp("DNS-01", args[1]) != 0))) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires a challenge type: HTTP-01 or DNS-01\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}

		if (alertif_too_many_args(2, file, linenum, args, &err_code))
			goto out;

		cur_acme->challenge = strdup(args[1]);
		if (!cur_acme->challenge) {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d]: out of memory.\n", file, linenum);
			goto out;
		}
	} else if (*args[0] != 0) {
		ha_alert("parsing [%s:%d]: unknown keyword '%s' in '%s' section\n", file, linenum, args[0], cursection);
		err_code |= ERR_ALERT | ERR_FATAL;
		goto out;
	}
out:
	free(errmsg);
	return err_code;
}

static int cfg_parse_acme_cfg_key(char **args, int section_type, struct proxy *curpx, const struct proxy *defpx,
                              const char *file, int linenum, char **err)
{
	int err_code = 0;
	char *errmsg = NULL;

	if (strcmp(args[0], "keytype") == 0) {
		if (!*args[1]) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires an argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
		if (alertif_too_many_args(1, file, linenum, args, &err_code))
			goto out;

		if (strcmp(args[1], "RSA") == 0) {
			cur_acme->key.type = EVP_PKEY_RSA;
		} else if (strcmp(args[1], "ECDSA") == 0) {
			cur_acme->key.type = EVP_PKEY_EC;
		} else {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires either 'RSA' or 'ECDSA' argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}

	} else if (strcmp(args[0], "bits") == 0) {
		char *stop;

		if (!*args[1]) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires an argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}

		cur_acme->key.bits = strtol(args[1], &stop, 10);
		if (*stop != '\0') {
			err_code |= ERR_ALERT | ERR_FATAL;
			ha_alert("parsing [%s:%d] : cannot parse '%s' value '%s', an integer is expected.\n", file, linenum, args[0], args[1]);
			goto out;
		}

		if (alertif_too_many_args(1, file, linenum, args, &err_code))
			goto out;

	} else if (strcmp(args[0], "curves") == 0) {
		if (!*args[1]) {
			ha_alert("parsing [%s:%d]: keyword '%s' in '%s' section requires an argument\n", file, linenum, args[0], cursection);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
		if (alertif_too_many_args(1, file, linenum, args, &err_code))
			goto out;

		if ((cur_acme->key.curves = curves2nid(args[1])) == -1) {
			ha_alert("parsing [%s:%d]: unsupported curves '%s'\n", file, linenum, args[1]);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto out;
		}
	}

out:
	free(errmsg);
	return err_code;
}

/* Initialize stuff once the section is parsed */
static int cfg_postsection_acme()
{
	struct acme_cfg *cur_acme = acme_cfgs;
	struct ckch_store *store;
	int err_code = 0;
	char *errmsg = NULL;
	char *path;
	struct stat st;

	/* TODO: generate a key at startup and dumps on the filesystem
	 * TODO: use the standard ckch loading for the account key (need a store with only a key)
	 */

	/* if account key filename is unspecified, choose a filename for it */
	if (!cur_acme->account.file) {
		if (!memprintf(&cur_acme->account.file, "%s.account.key", cur_acme->name)) {
			err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
			ha_alert("acme: out of memory.\n");
			goto out;
		}
	}

	path = cur_acme->account.file;

	store = ckch_store_new(path);
	if (!store) {
		ha_alert("acme: out of memory.\n");
		err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
		goto out;
	}
	/* tries to open the account key  */
	if (stat(path, &st) == 0) {
		if (ssl_sock_load_key_into_ckch(path, NULL, store->data, &errmsg)) {
			memprintf(&errmsg, "%s'%s' is present but cannot be read or parsed.\n", errmsg && *errmsg ? errmsg : NULL, path);
			if (errmsg && *errmsg)
				indent_msg(&errmsg, 8);
			err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
			ha_alert("acme: %s\n", errmsg);
			goto out;
		}
		/* ha_notice("acme: reading account key '%s' for id '%s'.\n", path, cur_acme->name); */
	} else {
		ha_alert("%s '%s' is not present and can't be generated, please provide an account file.\n", errmsg, path);
		err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
		goto out;
	}


	if (store->data->key == NULL) {
		ha_alert("acme: No Private Key found in '%s'.\n", path);
		err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
		goto out;
	}

	cur_acme->account.pkey = store->data->key;

	trash.data = jws_thumbprint(cur_acme->account.pkey, trash.area, trash.size);

	cur_acme->account.thumbprint = strndup(trash.area, trash.data);
	if (!cur_acme->account.thumbprint) {
		ha_alert("acme: out of memory.\n");
		err_code |= ERR_ALERT | ERR_FATAL | ERR_ABORT;
		goto out;
	}

	/* insert into the ckchs tree */
	ebst_insert(&ckchs_tree, &store->node);

out:
	ha_free(&errmsg);
	return err_code;
}

/* postparser function checks if the ACME section was declared */
static int cfg_postparser_acme()
{
	struct acme_cfg *tmp_acme = acme_cfgs;
	int ret = 0;

        /* first check if the ID was already used */
	while (tmp_acme) {
		/* if the linenum is not > 0, it means the acme keyword was used without declaring a section, and the
		 * linenum of the crt-store is stored negatively */
		if (tmp_acme->linenum <= 0) {
			ret++;
			ha_alert("acme '%s' was used on a crt line [%s:%d], but no '%s' section exists!\n",
			         tmp_acme->name, tmp_acme->filename, -tmp_acme->linenum, tmp_acme->name);
		}
		tmp_acme = tmp_acme->next;
	}


	return ret;
}

REGISTER_CONFIG_POSTPARSER("acme", cfg_postparser_acme);

void deinit_acme()
{
	struct acme_cfg *next = NULL;

	while (acme_cfgs) {

		next = acme_cfgs->next;
		ha_free(&acme_cfgs->name);
		ha_free(&acme_cfgs->uri);
		ha_free(&acme_cfgs->account.contact);
		ha_free(&acme_cfgs->account.file);
		ha_free(&acme_cfgs->account.thumbprint);

		free(acme_cfgs);
		acme_cfgs = next;
	}
}

static struct cfg_kw_list cfg_kws_acme = {ILH, {
	{ CFG_ACME, "uri",  cfg_parse_acme_kws },
	{ CFG_ACME, "contact",  cfg_parse_acme_kws },
	{ CFG_ACME, "account",  cfg_parse_acme_kws },
	{ CFG_ACME, "challenge",  cfg_parse_acme_kws },
	{ CFG_ACME, "keytype",  cfg_parse_acme_cfg_key },
	{ CFG_ACME, "bits",  cfg_parse_acme_cfg_key },
	{ CFG_ACME, "curves",  cfg_parse_acme_cfg_key },
	{ 0, NULL, NULL },
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws_acme);

REGISTER_CONFIG_SECTION("acme", cfg_parse_acme, cfg_postsection_acme);


static void acme_httpclient_end(struct httpclient *hc)
{
	struct task *task = hc->caller;
	struct acme_ctx *ctx = task->context;

	if (!task)
		return;

	if (ctx->http_state == ACME_HTTP_REQ)
		ctx->http_state = ACME_HTTP_RES;

	task_wakeup(task, TASK_WOKEN_MSG);
}


int acme_http_req(struct task *task, struct acme_ctx *ctx, struct ist url, enum http_meth_t meth, const struct http_hdr *hdrs, struct ist payload)
{
	struct httpclient *hc;

	hc = httpclient_new(task, meth, url);
	if (!hc)
		goto error;

	if (httpclient_req_gen(hc, hc->req.url, hc->req.meth, hdrs, payload) != ERR_NONE)
		goto error;

	hc->ops.res_end = acme_httpclient_end;

	ctx->hc = hc;

	if (!httpclient_start(hc))
		goto error;

	return 0;
error:
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return 1;

}

int acme_jws_payload(struct buffer *req, struct ist nonce, struct ist url, EVP_PKEY *pkey, struct ist kid, struct buffer *output, char **errmsg)
{
	struct buffer *b64payload = NULL;
	struct buffer *b64prot = NULL;
	struct buffer *b64sign = NULL;
	struct buffer *jwk = NULL;
	enum jwt_alg alg = JWS_ALG_NONE;
	int ret = 1;


	b64payload = alloc_trash_chunk();
	b64prot = alloc_trash_chunk();
	jwk = alloc_trash_chunk();
	b64sign = alloc_trash_chunk();

	if (!b64payload || !b64prot || !jwk || !b64sign || !output) {
		memprintf(errmsg, "out of memory");
		goto error;
	}

	if (!isttest(kid))
		jwk->data = EVP_PKEY_to_pub_jwk(pkey, jwk->area, jwk->size);
	alg = EVP_PKEY_to_jws_alg(pkey);

	if (alg == JWS_ALG_NONE) {
		memprintf(errmsg, "couldn't chose a JWK algorithm");
		goto error;
	}

	b64payload->data = jws_b64_payload(req->area, b64payload->area, b64payload->size);
	b64prot->data = jws_b64_protected(alg, kid.ptr, jwk->area, nonce.ptr, url.ptr, b64prot->area, b64prot->size);
	b64sign->data = jws_b64_signature(pkey, alg, b64prot->area, b64payload->area, b64sign->area, b64sign->size);
	output->data = jws_flattened(b64prot->area, b64payload->area, b64sign->area, output->area, output->size);

	if (output->data == 0)
		goto error;

	ret = 0;

error:
	free_trash_chunk(b64sign);
	free_trash_chunk(jwk);
	free_trash_chunk(b64prot);
	free_trash_chunk(b64payload);


	return ret;
}

/*
 * Update every certificate instances for the new store
 *
 * XXX: ideally this should be reentrant like in lua or the CLI.
 */
int acme_update_certificate(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	int ret = 1;
	struct ckch_store *old_ckchs, *new_ckchs = NULL;
	struct ckch_inst *ckchi;

	new_ckchs = ctx->store;

	if (HA_SPIN_TRYLOCK(CKCH_LOCK, &ckch_lock)) {
		memprintf(errmsg, "couldn't get the certificate lock!");
		goto error;

	}

	if ((old_ckchs = ckchs_lookup(new_ckchs->path)) == NULL) {
		memprintf(errmsg, "couldn't find the previous certificate to update");
		goto error;
	}

	ckchi = LIST_ELEM(old_ckchs->ckch_inst.n, typeof(ckchi), by_ckchs);

	/* walk through the old ckch_inst and creates new ckch_inst using the updated ckchs */
	list_for_each_entry_from(ckchi, &old_ckchs->ckch_inst, by_ckchs) {
		struct ckch_inst *new_inst;

		if (ckch_inst_rebuild(new_ckchs, ckchi, &new_inst, errmsg)) {
			goto error;
		}

		/* link the new ckch_inst to the duplicate */
		LIST_APPEND(&new_ckchs->ckch_inst, &new_inst->by_ckchs);
	}

	/* insert everything and remove the previous objects */
	ckch_store_replace(old_ckchs, new_ckchs);

	ret = 0;

error:
	HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);
	return ret;

}

int acme_res_certificate(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;
	EVP_PKEY *key;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300) {
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.type", t2->area, t2->size)) > -1)
			t2->data = ret;

		if (t2->data && t1->data)
			memprintf(errmsg, "invalid HTTP status code %d when getting challenge URL: \"%.*s\" (%.*s)", hc->res.status, (int)t1->data, t1->area, (int)t2->data, t2->area);
		else
			memprintf(errmsg, "invalid HTTP status code %d when getting challengge URL", hc->res.status);
		goto error;
	}

	/* loading a PEM would remove the key, save it for later */
	key = ctx->store->data->key;
	ctx->store->data->key = NULL;

	/* XXX: might need a function dedicated to this, which does not read a private key */
	if (ssl_sock_load_pem_into_ckch(ctx->store->path, hc->res.buf.area, ctx->store->data , errmsg) != 0)
		goto error;

	/* restore the key */
	ctx->store->data->key = key;

	if (acme_update_certificate(task, ctx, errmsg) != 0)
		goto error;

out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}

int acme_res_chkorder(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; hdrs && isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300) {
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.type", t2->area, t2->size)) > -1)
			t2->data = ret;

		if (t2->data && t1->data)
			memprintf(errmsg, "invalid HTTP status code %d when getting Order URL: \"%.*s\" (%.*s)", hc->res.status, (int)t1->data, t1->area, (int)t2->data, t2->area);
		else
			memprintf(errmsg, "invalid HTTP status code %d when getting Order URL", hc->res.status);
		goto error;
	}
	ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.certificate", trash.area, trash.size);
	if (ret == -1) {
		memprintf(errmsg, "couldn't get a the certificate URL");
		goto error;
	}
	trash.data = ret;
	ctx->certificate = istdup(ist2(trash.area, trash.data));
	if (!isttest(ctx->certificate)) {
		memprintf(errmsg, "out of memory");
		goto error;
	}
	ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.status", trash.area, trash.size);
	if (ret == -1) {
		memprintf(errmsg, "couldn't get a the Order status");
		goto error;
	}
	trash.data = ret;
	if (strncasecmp("valid", trash.area, trash.data) != 0) {
		memprintf(errmsg, "order status: %.*s", (int)trash.data, trash.area);
		goto error;
	};

out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}

/* Send the CSR over the Finalize URL */
int acme_req_finalize(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	X509_REQ *req = ctx->req;
	struct buffer *csr = NULL;
	struct buffer *req_in = NULL;
	struct buffer *req_out = NULL;
	const struct http_hdr hdrs[] = {
		{ IST("Content-Type"), IST("application/jose+json") },
		{ IST_NULL, IST_NULL }
	};
	int ret = 1;
	size_t len = 0;
	unsigned char *data = NULL;

        if ((csr = alloc_trash_chunk()) == NULL)
		goto error;
        if ((req_in = alloc_trash_chunk()) == NULL)
		goto error;
        if ((req_out = alloc_trash_chunk()) == NULL)
		goto error;

	len = i2d_X509_REQ(req, &data);
	if (len <= 0)
		goto error;

	ret = a2base64url((char *)data, len, csr->area, csr->size);
	if (ret <= 0)
		goto error;
	csr->data = ret;

	chunk_printf(req_in, "{ \"csr\": \"%.*s\" }", (int)csr->data, csr->area);
	free(data);


	if (acme_jws_payload(req_in, ctx->nonce, ctx->finalize, ctx->cfg->account.pkey, ctx->kid, req_out, errmsg) != 0)
		goto error;

	if (acme_http_req(task, ctx, ctx->finalize, HTTP_METH_POST, hdrs, ist2(req_out->area, req_out->data)))
		goto error;


	ret = 0;
error:
	memprintf(errmsg, "couldn't request the finalize URL");

	free_trash_chunk(req_in);
	free_trash_chunk(req_out);
	free_trash_chunk(csr);

	return ret;

}

int acme_res_finalize(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300) {
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.type", t2->area, t2->size)) > -1)
			t2->data = ret;

		if (t2->data && t1->data)
			memprintf(errmsg, "invalid HTTP status code %d when getting Finalize URL: \"%.*s\" (%.*s)", hc->res.status, (int)t1->data, t1->area, (int)t2->data, t2->area);
		else
			memprintf(errmsg, "invalid HTTP status code %d when getting Finalize URL", hc->res.status);
		goto error;
	}
out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}

/*
 * Send the READY request for the challenge
 */
int acme_req_challenge(struct task *task, struct acme_ctx *ctx, struct acme_auth *auth, char **errmsg)
{
	struct buffer *req_in = NULL;
	struct buffer *req_out = NULL;
	const struct http_hdr hdrs[] = {
		{ IST("Content-Type"), IST("application/jose+json") },
		{ IST_NULL, IST_NULL }
	};
	int ret = 1;

        if ((req_in = alloc_trash_chunk()) == NULL)
		goto error;
        if ((req_out = alloc_trash_chunk()) == NULL)
		goto error;

	chunk_printf(req_in, "{}");

	if (acme_jws_payload(req_in, ctx->nonce, auth->chall, ctx->cfg->account.pkey, ctx->kid, req_out, errmsg) != 0)
		goto error;

	if (acme_http_req(task, ctx, auth->chall, HTTP_METH_POST, hdrs, ist2(req_out->area, req_out->data)))
		goto error;

	ret = 0;
error:
	memprintf(errmsg, "couldn't generate the Challenge request");

	free_trash_chunk(req_in);
	free_trash_chunk(req_out);

	return ret;

}

/* parse the challenge URL response */
int acme_res_challenge(struct task *task, struct acme_ctx *ctx, struct acme_auth *auth, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300 || mjson_find(hc->res.buf.area, hc->res.buf.data, "$.error", NULL, NULL) == MJSON_TOK_OBJECT) {
		/* XXX: need a generic URN error parser */
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.error.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.error.type", t2->area, t2->size)) > -1)
			t2->data = ret;
		if (t2->data && t1->data)
			memprintf(errmsg, "error when when getting Challenge URL: \"%.*s\" (%.*s) (HTTP status code %d)", (int)t1->data, t1->area, (int)t2->data, t2->area, hc->res.status);
		else
			memprintf(errmsg, "error when getting Challenge URL (HTTP status code %d)", hc->res.status);
		goto error;
	}

out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}


/*
 * Get an Auth URL
 */
int acme_req_auth(struct task *task, struct acme_ctx *ctx, struct acme_auth *auth, char **errmsg)
{
	struct buffer *req_in = NULL;
	struct buffer *req_out = NULL;
	const struct http_hdr hdrs[] = {
		{ IST("Content-Type"), IST("application/jose+json") },
		{ IST_NULL, IST_NULL }
	};
	int ret = 1;

        if ((req_in = alloc_trash_chunk()) == NULL)
		goto error;
        if ((req_out = alloc_trash_chunk()) == NULL)
		goto error;

	/* empty payload */
	if (acme_jws_payload(req_in, ctx->nonce, auth->auth, ctx->cfg->account.pkey, ctx->kid, req_out, errmsg) != 0)
		goto error;

	if (acme_http_req(task, ctx, auth->auth, HTTP_METH_POST, hdrs, ist2(req_out->area, req_out->data)))
		goto error;

	ret = 0;
error:
	memprintf(errmsg, "couldn't generate the Authorizations request");

	free_trash_chunk(req_in);
	free_trash_chunk(req_out);

	return ret;

}

int acme_res_auth(struct task *task, struct acme_ctx *ctx, struct acme_auth *auth, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;
	int i;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300) {
		/* XXX: need a generic URN error parser */
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.type", t2->area, t2->size)) > -1)
			t2->data = ret;
		if (t2->data && t1->data)
			memprintf(errmsg, "invalid HTTP status code %d when getting Authorization URL: \"%.*s\" (%.*s)", hc->res.status, (int)t1->data, t1->area, (int)t2->data, t2->area);
		else
			memprintf(errmsg, "invalid HTTP status code %d when getting Authorization URL", hc->res.status);
		goto error;
	}

	/* get the multiple challenges and select the one from the configuration */
	for (i = 0; ; i++) {
		int ret;
		char chall[] = "$.challenges[XXX]";
		const char *tokptr;
		int toklen;

		if (snprintf(chall, sizeof(chall), "$.challenges[%d]", i) >= sizeof(chall))
			goto error;

		/* break the loop at the end of the challenges objects list */
		if (mjson_find(hc->res.buf.area, hc->res.buf.data, chall, &tokptr, &toklen) == MJSON_TOK_INVALID)
			break;

		ret = mjson_get_string(tokptr, toklen, "$.type", trash.area, trash.size);
		if (ret == -1) {
			memprintf(errmsg, "couldn't get a challenge type in challenges[%d] from Authorization URL \"%s\"", i, auth->auth.ptr);
			goto error;
		}
		trash.data = ret;

		/* skip until this is the challenge we need */
		if (strncasecmp(ctx->cfg->challenge, trash.area, trash.data) != 0)
			continue;

		ret = mjson_get_string(tokptr, toklen, "$.url", trash.area, trash.size);
		if (ret == -1) {
			memprintf(errmsg, "couldn't get a challenge URL in challenges[%d] from Authorization URL \"%s\"", i, auth->auth.ptr);
			goto error;
		}
		trash.data = ret;
		auth->chall = istdup(ist2(trash.area, trash.data));
		if (!isttest(auth->chall)) {
			memprintf(errmsg, "out of memory");
			goto error;
		}

		ret = mjson_get_string(tokptr, toklen, "$.token", trash.area, trash.size);
		if (ret == -1) {
			memprintf(errmsg, "couldn't get a token in challenges[%d] from Authorization URL \"%s\"", i, auth->auth.ptr);
			goto error;
		}
		trash.data = ret;
		auth->token = istdup(ist2(trash.area, trash.data));
		if (!isttest(auth->token)) {
			memprintf(errmsg, "out of memory");
			goto error;
		}

		/* we only need one challenge, and iteration is only used to found the right one */
		break;
	}

out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}


int acme_req_neworder(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct buffer *req_in = NULL;
	struct buffer *req_out = NULL;
	const struct http_hdr hdrs[] = {
		{ IST("Content-Type"), IST("application/jose+json") },
		{ IST_NULL, IST_NULL }
	};
	int ret = 1;
	char **san = ctx->store->conf.acme.domains;

        if ((req_in = alloc_trash_chunk()) == NULL)
		goto error;
        if ((req_out = alloc_trash_chunk()) == NULL)
		goto error;

	chunk_printf(req_in, "{ \"identifiers\": [ ");

	if (!san)
		goto error;

	for (; san && *san; san++) {
//		fprintf(stderr, "%s:%d %s\n", __FUNCTION__, __LINE__, *san);
		chunk_appendf(req_in, "%s{ \"type\": \"dns\",  \"value\": \"%s\" }", (*san == *ctx->store->conf.acme.domains) ?  "" : ",", *san);
	}

	chunk_appendf(req_in, " ] }");


	if (acme_jws_payload(req_in, ctx->nonce, ctx->ressources.newOrder, ctx->cfg->account.pkey, ctx->kid, req_out, errmsg) != 0)
		goto error;

	if (acme_http_req(task, ctx, ctx->ressources.newOrder, HTTP_METH_POST, hdrs, ist2(req_out->area, req_out->data)))
		goto error;

	ret = 0;
error:
	memprintf(errmsg, "couldn't generate the newOrder request");

	free_trash_chunk(req_in);
	free_trash_chunk(req_out);

	return ret;

}

int acme_res_neworder(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;
	int i;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
		/* get the order URL */
		if (isteqi(hdr->n, ist("Location"))) {
			istfree(&ctx->order);
			ctx->order = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300) {
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.type", t2->area, t2->size)) > -1)
			t2->data = ret;
		if (t2->data && t1->data)
			memprintf(errmsg, "invalid HTTP status code %d when getting newOrder URL: \"%.*s\" (%.*s)", hc->res.status, (int)t1->data, t1->area, (int)t2->data, t2->area);
		else
			memprintf(errmsg, "invalid HTTP status code %d when getting newOrder URL", hc->res.status);
		goto error;
	}

	if (!isttest(ctx->order)) {
		memprintf(errmsg, "couldn't get an order Location during newOrder");
		goto error;
	}
	/* get the multiple authorizations URL and tokens */
	for (i = 0; ; i++) {
		struct acme_auth *auth;
		char url[] = "$.authorizations[XXX]";

		if (snprintf(url, sizeof(url), "$.authorizations[%d]", i) >= sizeof(url)) {
			memprintf(errmsg, "couldn't loop on authorizations during newOrder");
			goto error;
		}

		ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, url, trash.area, trash.size);
		if (ret == -1) /* end of the authorizations array */
			break;
		trash.data = ret;

		if ((auth = calloc(1, sizeof(*auth))) == NULL) {
			memprintf(errmsg, "out of memory");
			goto error;
		}

		auth->auth = istdup(ist2(trash.area, trash.data));
		if (!isttest(auth->auth)) {
			memprintf(errmsg, "out of memory");
			goto error;
		}

		auth->next = ctx->auths;
		ctx->auths = auth;
		ctx->next_auth = auth;
	}

	if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.finalize", trash.area, trash.size)) <= 0) {
		memprintf(errmsg, "couldn't find the finalize URL");
		goto error;
	}
	trash.data = ret;
	istfree(&ctx->finalize);
	ctx->finalize = istdup(ist2(trash.area, trash.data));
	if (!isttest(ctx->finalize)) {
		memprintf(errmsg, "out of memory");
		goto error;
	}

out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}


int acme_req_account(struct task *task, struct acme_ctx *ctx, int newaccount, char **errmsg)
{
	struct buffer *req_in = NULL;
	struct buffer *req_out = NULL;
	const struct http_hdr hdrs[] = {
		{ IST("Content-Type"), IST("application/jose+json") },
		{ IST_NULL, IST_NULL }
	};
	char *accountreq = "{\n"
		"    \"termsOfServiceAgreed\": true,\n"
		"    \"onlyReturnExisting\":   true\n"
		"}\n";
	char *newaccountreq = "{\n"
		"    \"termsOfServiceAgreed\": true,\n"
		"    \"contact\": [\n"
		"        \"mailto:%s\"\n"
		"    ]\n"
		"}\n";
	int ret = 1;

        if ((req_in = alloc_trash_chunk()) == NULL)
		goto error;
        if ((req_out = alloc_trash_chunk()) == NULL)
		goto error;

	if (newaccount)
		chunk_printf(req_in, newaccountreq, ctx->cfg->account.contact);
	else
		chunk_printf(req_in, "%s", accountreq);

	if (acme_jws_payload(req_in, ctx->nonce, ctx->ressources.newAccount, ctx->cfg->account.pkey, ctx->kid, req_out, errmsg) != 0)
		goto error;

	if (acme_http_req(task, ctx, ctx->ressources.newAccount, HTTP_METH_POST, hdrs, ist2(req_out->area, req_out->data)))
		goto error;

	ret = 0;
error:
	memprintf(errmsg, "couldn't generate the newAccount request");

	free_trash_chunk(req_in);
	free_trash_chunk(req_out);

	return ret;
}

int acme_res_account(struct task *task, struct acme_ctx *ctx, int newaccount, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;
	struct buffer *t1 = NULL, *t2 = NULL;
	int ret = 1;

	hc = ctx->hc;
	if (!hc)
		goto error;

        if ((t1 = alloc_trash_chunk()) == NULL)
		goto error;
        if ((t2 = alloc_trash_chunk()) == NULL)
		goto error;

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Location"))) {
			istfree(&ctx->kid);
			ctx->kid = istdup(hdr->v);
		}
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
		}
	}

	if (hc->res.status < 200 || hc->res.status >= 300) {
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.detail", t1->area, t1->size)) > -1)
			t1->data = ret;
		if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.type", t2->area, t2->size)) > -1)
			t2->data = ret;

		if (!newaccount) {
			/* not an error, we only need to create a new account */
			if (strcmp("urn:ietf:params:acme:error:accountDoesNotExist", t2->area) == 0)
				goto out;
		}

		if (t2->data && t1->data)
			memprintf(errmsg, "invalid HTTP status code %d when getting Account URL: \"%.*s\" (%.*s)", hc->res.status, (int)t1->data, t1->area, (int)t2->data, t2->area);
		else
			memprintf(errmsg, "invalid HTTP status code %d when getting Account URL", hc->res.status);
		goto error;
	}
out:
	ret = 0;

error:
	free_trash_chunk(t1);
	free_trash_chunk(t2);
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return ret;
}



int acme_nonce(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct httpclient *hc;
	struct http_hdr *hdrs, *hdr;

	hc = ctx->hc;
	if (!hc)
		goto error;

	if (hc->res.status < 200 || hc->res.status >= 300) {
		memprintf(errmsg, "invalid HTTP status code %d when getting Nonce URL", hc->res.status);
		goto error;
	}

	hdrs = hc->res.hdrs;

	for (hdr = hdrs; isttest(hdr->v); hdr++) {
		if (isteqi(hdr->n, ist("Replay-Nonce"))) {
			istfree(&ctx->nonce);
			ctx->nonce = istdup(hdr->v);
//			fprintf(stderr, "Replay-Nonce: %.*s\n", (int)hdr->v.len, hdr->v.ptr);

		}
	}

	httpclient_destroy(hc);
	ctx->hc = NULL;

	return 0;

error:
	httpclient_destroy(hc);
	ctx->hc = NULL;

	return 1;
}

int acme_directory(struct task *task, struct acme_ctx *ctx, char **errmsg)
{
	struct httpclient *hc;
	int ret = 0;

	hc = ctx->hc;

	if (!hc)
		goto error;

	if (hc->res.status != 200) {
		memprintf(errmsg, "invalid HTTP status code %d when getting directory URL", hc->res.status);
		goto error;
	}

	if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.newNonce", trash.area, trash.size)) <= 0) {
		memprintf(errmsg, "couldn't get newNonce URL from the directory URL");
		goto error;
	}
	ctx->ressources.newNonce = istdup(ist2(trash.area, ret));
	if (!isttest(ctx->ressources.newNonce)) {
		memprintf(errmsg, "couldn't get newNonce URL from the directory URL");
		goto error;
	}

	if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.newAccount", trash.area, trash.size)) <= 0) {
		memprintf(errmsg, "couldn't get newAccount URL from the directory URL");
		goto error;
	}
	ctx->ressources.newAccount = istdup(ist2(trash.area, ret));
	if (!isttest(ctx->ressources.newAccount)) {
		memprintf(errmsg, "couldn't get newAccount URL from the directory URL");
		goto error;
	}
	if ((ret = mjson_get_string(hc->res.buf.area, hc->res.buf.data, "$.newOrder", trash.area, trash.size)) <= 0) {
		memprintf(errmsg, "couldn't get newOrder URL from the directory URL");
		goto error;
	}
	ctx->ressources.newOrder = istdup(ist2(trash.area, ret));
	if (!isttest(ctx->ressources.newOrder)) {
		memprintf(errmsg, "couldn't get newOrder URL from the directory URL");
		goto error;
	}

	httpclient_destroy(hc);
	ctx->hc = NULL;

//	fprintf(stderr, "newNonce: %s\nnewAccount: %s\nnewOrder: %s\n",
//	        ctx->ressources.newNonce.ptr, ctx->ressources.newAccount.ptr, ctx->ressources.newOrder.ptr);

	return 0;

error:
	httpclient_destroy(hc);
	ctx->hc = NULL;

	istfree(&ctx->ressources.newNonce);
	istfree(&ctx->ressources.newAccount);
	istfree(&ctx->ressources.newOrder);

	return 1;
}

/*
 * Task for ACME processing:
 *  - when retrying after a failure, the task must be waked up
 *  - when calling a get function, the httpclient is waking up the task again
 * once the data are ready or upon failure
 */
struct task *acme_process(struct task *task, void *context, unsigned int state)
{
	struct acme_ctx *ctx = task->context;
	enum acme_st st = ctx->state;
	enum http_st http_st = ctx->http_state;
	char *errmsg = NULL;

	switch (st) {
		case ACME_RESSOURCES:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_http_req(task, ctx, ist(ctx->cfg->uri), HTTP_METH_GET, NULL, IST_NULL) != 0)
					goto retry;
			}

			if (http_st == ACME_HTTP_RES) {
				if (acme_directory(task, ctx, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				st = ACME_NEWNONCE;
				http_st = ACME_HTTP_REQ;
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_NEWNONCE:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_http_req(task, ctx, ctx->ressources.newNonce, HTTP_METH_HEAD, NULL, IST_NULL) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_nonce(task, ctx, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				st = ACME_CHKACCOUNT;
				http_st = ACME_HTTP_REQ;
				task_wakeup(task, TASK_WOKEN_MSG);
			}

		break;
		case ACME_CHKACCOUNT:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_req_account(task, ctx, 0, &errmsg) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_account(task, ctx, 0, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				if (!isttest(ctx->kid))
					st = ACME_NEWACCOUNT;
				else
					st = ACME_NEWORDER;
				http_st = ACME_HTTP_REQ;
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_NEWACCOUNT:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_req_account(task, ctx, 1, &errmsg) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_account(task, ctx, 1, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				st = ACME_NEWORDER;
				http_st = ACME_HTTP_REQ;
				task_wakeup(task, TASK_WOKEN_MSG);
				goto end;
			}


		break;
		case ACME_NEWORDER:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_req_neworder(task, ctx, &errmsg) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_neworder(task, ctx, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				st = ACME_AUTH;
				http_st = ACME_HTTP_REQ;
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_AUTH:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_req_auth(task, ctx, ctx->next_auth, &errmsg) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_auth(task, ctx, ctx->next_auth, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				http_st = ACME_HTTP_REQ;
				if ((ctx->next_auth = ctx->next_auth->next) == NULL) {
					st = ACME_CHALLENGE;
					ctx->next_auth = ctx->auths;
				}
				/* call with next auth or do the challenge step */
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_CHALLENGE:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_req_challenge(task, ctx, ctx->next_auth, &errmsg) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_challenge(task, ctx, ctx->next_auth, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				http_st = ACME_HTTP_REQ;
				if ((ctx->next_auth = ctx->next_auth->next) == NULL) {
					st = ACME_CHKCHALLENGE;
					ctx->next_auth = ctx->auths;
				}
				/* call with next auth or do the challenge step */
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_CHKCHALLENGE:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_http_req(task, ctx, ctx->next_auth->chall, HTTP_METH_GET, NULL, IST_NULL) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_challenge(task, ctx, ctx->next_auth, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				http_st = ACME_HTTP_REQ;
				if ((ctx->next_auth = ctx->next_auth->next) == NULL)
					st = ACME_FINALIZE;

				/* do it with the next auth or finalize */
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_FINALIZE:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_req_finalize(task, ctx, &errmsg) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_finalize(task, ctx, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				http_st = ACME_HTTP_REQ;
				st = ACME_CHKORDER;
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_CHKORDER:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_http_req(task, ctx, ctx->order, HTTP_METH_GET, NULL, IST_NULL) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_chkorder(task, ctx, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				http_st = ACME_HTTP_REQ;
				st = ACME_CERTIFICATE;
				task_wakeup(task, TASK_WOKEN_MSG);
			}
		break;
		case ACME_CERTIFICATE:
			if (http_st == ACME_HTTP_REQ) {
				if (acme_http_req(task, ctx, ctx->certificate, HTTP_METH_GET, NULL, IST_NULL) != 0)
					goto retry;
			}
			if (http_st == ACME_HTTP_RES) {
				if (acme_res_certificate(task, ctx, &errmsg) != 0) {
					http_st = ACME_HTTP_REQ;
					goto retry;
				}
				http_st = ACME_HTTP_REQ;
				goto end;
			}
		break;

		case ACME_END:
			goto end;
		break;

	}

	ctx->http_state = http_st;
	ctx->state = st;

	return task;

retry:
	ctx->http_state = http_st;
	ctx->state = st;

	ctx->retries--;
	if (ctx->retries > 0) {
		ha_notice("acme: %s, retrying (%d/%d)...\n", errmsg ? errmsg : "", ACME_RETRY-ctx->retries, ACME_RETRY);
		task_wakeup(task, TASK_WOKEN_MSG);
	} else {
		ha_notice("acme: %s, aborting. (%d/%d)\n", errmsg ? errmsg : "", ACME_RETRY-ctx->retries, ACME_RETRY);
		goto end;
	}

	ha_free(&errmsg);

	return task;
end:
	task_destroy(task);
	task = NULL;

	return task;
}

/*
 * Generate a X509_REQ using a PKEY and a list of SAN finished by a NULL entry
 */
X509_REQ *acme_x509_req(EVP_PKEY *pkey, char **san)
{
	struct buffer *san_trash = NULL;
	X509_REQ *x = NULL;
	X509_NAME *nm;
	STACK_OF(X509_EXTENSION) *exts = NULL;
	X509_EXTENSION *ext_san;
	char *str_san = NULL;
	int i = 0;

	if ((san_trash = alloc_trash_chunk()) == NULL)
		goto error;

	if ((x = X509_REQ_new()) == NULL)
		goto error;

	if (!X509_REQ_set_pubkey(x, pkey))
		goto error;

	if ((nm = X509_NAME_new()) == NULL)
		goto error;

	/* common name is the first SAN in the list */
	if (!X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
	                         (unsigned char *)san[0], -1, -1, 0))
		goto error;
	/* assign the CN to the REQ */
	if (!X509_REQ_set_subject_name(x, nm))
		goto error;

	/* Add the SANs */
	if ((exts = sk_X509_EXTENSION_new_null()) == NULL)
		goto error;

	for (i = 0; san[i]; i++) {
		chunk_appendf(san_trash, "%sDNS:%s", i ? "," : "", san[i]);
	}
	str_san = strndup(san_trash->area, san_trash->data);

	if ((ext_san = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, str_san)) == NULL)
		goto error;

	if (!sk_X509_EXTENSION_push(exts, ext_san))
		goto error;
	if (!X509_REQ_add_extensions(x, exts))
		goto error;

	sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);

	if (!X509_REQ_sign(x, pkey, EVP_sha256()))
		goto error;

	free_trash_chunk(san_trash);

	return x;

error:
	free_trash_chunk(san_trash);
	return NULL;

}

static int cli_acme_renew_parse(char **args, char *payload, struct appctx *appctx, void *private)
{
	char *err = NULL;
	struct acme_cfg *cfg;
	struct task *task;
	struct acme_ctx *ctx = NULL;
	struct ckch_store *store = NULL, *newstore = NULL;
	EVP_PKEY_CTX *pkey_ctx = NULL;
	EVP_PKEY *pkey = NULL;

	if (!*args[1]) {
		memprintf(&err, ": not enough parameters\n");
		goto err;
	}

	if (HA_SPIN_TRYLOCK(CKCH_LOCK, &ckch_lock))
		return cli_err(appctx, "Can't update: operations on certificates are currently locked!\n");

	if ((store = ckchs_lookup(args[2])) == NULL) {
		memprintf(&err, "Can't find the certificate '%s'.\n", args[1]);
		goto err;
	}

	if (store->conf.acme.id == NULL) {
		memprintf(&err, "No ACME configuration defined for file '%s'.\n", args[1]);
		goto err;
	}

	cfg = get_acme_cfg(store->conf.acme.id);
	if (!cfg) {
		memprintf(&err, "No ACME configuration found for file '%s'.\n", args[1]);
		goto err;
	}

	newstore = ckchs_dup(store);
	if (!newstore) {
		memprintf(&err, "Out of memory.\n");
		goto err;
	}

	HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);

	ctx = calloc(1, sizeof *ctx);
	if (!ctx) {
		memprintf(&err, "Out of memory.\n");
		goto err;
	}

	/* set the number of remaining retries when facing an error */
	ctx->retries = ACME_RETRY;

	if ((pkey_ctx = EVP_PKEY_CTX_new_id(cfg->key.type, NULL)) == NULL) {
		memprintf(&err, "%sCan't generate a private key.\n", err ? err : "");
		goto err;
	}

	if (EVP_PKEY_keygen_init(pkey_ctx) <= 0) {
		memprintf(&err, "%sCan't generate a private key.\n", err ? err : "");
		goto err;
	}

	if (cfg->key.type == EVP_PKEY_EC) {
		if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pkey_ctx, cfg->key.curves) <= 0) {
			memprintf(&err, "%sCan't set the curves on the new private key.\n", err ? err : "");
			goto err;
		}
	} else if (cfg->key.type == EVP_PKEY_RSA) {
		if (EVP_PKEY_CTX_set_rsa_keygen_bits(pkey_ctx, cfg->key.bits) <= 0) {
			memprintf(&err, "%sCan't set the bits on the new private key.\n", err ? err : "");
			goto err;
		}
	}

	if (EVP_PKEY_keygen(pkey_ctx, &pkey) <= 0) {
		memprintf(&err, "%sCan't generate a private key.\n", err ? err : "");
		goto err;
	}

	EVP_PKEY_CTX_free(pkey_ctx);

	EVP_PKEY_free(newstore->data->key);
	newstore->data->key = pkey;

	ctx->req = acme_x509_req(pkey, store->conf.acme.domains);
	if (!ctx->req) {
		memprintf(&err, "%sCan't generate a CSR.\n", err ? err : "");
		goto err;
	}


	ctx->store = newstore;
	ctx->cfg = cfg;

	task = task_new_anywhere();
	if (!task)
		goto err;
	task->nice = 0;
	task->process = acme_process;
	task->context = ctx;

	task_wakeup(task, TASK_WOKEN_INIT);

	return 0;

err:
	HA_SPIN_UNLOCK(CKCH_LOCK, &ckch_lock);
	ckch_store_free(newstore);
	EVP_PKEY_CTX_free(pkey_ctx);
	free(ctx);
	memprintf(&err, "%sCan't start the ACME client.\n", err ? err : "");
	return cli_dynerr(appctx, err);
}



static struct cli_kw_list cli_kws = {{ },{
	{ { "acme", "renew", NULL }, NULL, cli_acme_renew_parse, NULL, NULL, NULL, 0 },
	{ { NULL }, NULL, NULL, NULL }
}};


INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
