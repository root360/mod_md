/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <assert.h>
#include <stdio.h>

#include <apr_lib.h>
#include <apr_buckets.h>
#include <apr_file_info.h>
#include <apr_file_io.h>
#include <apr_fnmatch.h>
#include <apr_hash.h>
#include <apr_strings.h>
#include <apr_tables.h>

#include "md.h"
#include "md_crypt.h"
#include "md_json.h"
#include "md_http.h"
#include "md_log.h"
#include "md_jws.h"
#include "md_store.h"
#include "md_util.h"

#include "md_acme.h"
#include "md_acme_authz.h"
#include "md_acme_order.h"


md_acme_order_t *md_acme_order_create(apr_pool_t *p)
{
    md_acme_order_t *order;
    
    order = apr_pcalloc(p, sizeof(*order));
    order->p = p;
    order->authz_urls = apr_array_make(p, 5, sizeof(const char *));
    order->challenge_dirs = apr_array_make(p, 5, sizeof(const char *));
    
    return order;
}

/**************************************************************************************************/
/* order conversion */

#define MD_KEY_AUTHZS           "authorizations"
#define MD_KEY_CHALLENGE_DIRS   "challenge-dirs"

md_json_t *md_acme_order_to_json(md_acme_order_t *order, apr_pool_t *p)
{
    md_json_t *json = md_json_create(p);

    if (order->url) {
        md_json_sets(order->url, json, MD_KEY_URL, NULL);
    }
    md_json_setsa(order->authz_urls, json, MD_KEY_AUTHZS, NULL);
    md_json_setsa(order->challenge_dirs, json, MD_KEY_CHALLENGE_DIRS, NULL);
    return json;
}

md_acme_order_t *md_acme_order_from_json(md_json_t *json, apr_pool_t *p)
{
    md_acme_order_t *order = md_acme_order_create(p);

    order->url = md_json_gets(json, MD_KEY_URL, NULL);
    md_json_getsa(order->authz_urls, json, MD_KEY_AUTHZS, NULL);
    md_json_getsa(order->challenge_dirs, json, MD_KEY_CHALLENGE_DIRS, NULL);
    return order;
}

apr_status_t md_acme_order_add(md_acme_order_t *order, const char *authz_url)
{
    assert(authz_url);
    if (md_array_str_index(order->authz_urls, authz_url, 0, 1) < 0) {
        APR_ARRAY_PUSH(order->authz_urls, const char*) = apr_pstrdup(order->p, authz_url);
    }
    return APR_SUCCESS;
}

apr_status_t md_acme_order_remove(md_acme_order_t *order, const char *authz_url)
{
    int i;
    
    assert(authz_url);
    i = md_array_str_index(order->authz_urls, authz_url, 0, 1);
    if (i >= 0) {
        order->authz_urls = md_array_str_remove(order->p, order->authz_urls, authz_url, 1);
        return APR_SUCCESS;
    }
    return APR_ENOENT;
}

apr_status_t md_acme_order_add_challenge_dir(md_acme_order_t *order, const char *dir)
{
    if (dir && (md_array_str_index(order->challenge_dirs, dir, 0, 1) < 0)) {
        APR_ARRAY_PUSH(order->challenge_dirs, const char*) = apr_pstrdup(order->p, dir);
    }
    return APR_SUCCESS;
}

/**************************************************************************************************/
/* persistence */

apr_status_t md_acme_order_load(struct md_store_t *store, md_store_group_t group, 
                                    const char *md_name, md_acme_order_t **pauthz_set, 
                                    apr_pool_t *p)
{
    apr_status_t rv;
    md_json_t *json;
    md_acme_order_t *authz_set;
    
    rv = md_store_load_json(store, group, md_name, MD_FN_ORDER, &json, p);
    if (APR_SUCCESS == rv) {
        authz_set = md_acme_order_from_json(json, p);
    }
    *pauthz_set = (APR_SUCCESS == rv)? authz_set : NULL;
    return rv;  
}

static apr_status_t p_save(void *baton, apr_pool_t *p, apr_pool_t *ptemp, va_list ap)
{
    md_store_t *store = baton;
    md_json_t *json;
    md_store_group_t group;
    md_acme_order_t *set;
    const char *md_name;
    int create;
 
    (void)p;   
    group = (md_store_group_t)va_arg(ap, int);
    md_name = va_arg(ap, const char *);
    set = va_arg(ap, md_acme_order_t *);
    create = va_arg(ap, int);

    json = md_acme_order_to_json(set, ptemp);
    assert(json);
    return md_store_save_json(store, ptemp, group, md_name, MD_FN_ORDER, json, create);
}

apr_status_t md_acme_order_save(struct md_store_t *store, apr_pool_t *p,
                                    md_store_group_t group, const char *md_name, 
                                    md_acme_order_t *authz_set, int create)
{
    return md_util_pool_vdo(p_save, store, p, group, md_name, authz_set, create, NULL);
}

static apr_status_t p_purge(void *baton, apr_pool_t *p, apr_pool_t *ptemp, va_list ap)
{
    md_store_t *store = baton;
    md_acme_order_t *order;
    md_store_group_t group;
    const char *md_name, *dir;
    int i;

    group = (md_store_group_t)va_arg(ap, int);
    md_name = va_arg(ap, const char *);

    if (APR_SUCCESS == md_acme_order_load(store, group, md_name, &order, p)) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, p, "order loaded for %s", md_name);
        for (i = 0; i < order->challenge_dirs->nelts; ++i) {
            dir = APR_ARRAY_IDX(order->challenge_dirs, i, const char*);
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, p, "order purge challenge at %s", dir);
            md_store_purge(store, p, MD_SG_CHALLENGES, dir);
        }
    }
    return md_store_remove(store, group, md_name, MD_FN_ORDER, ptemp, 1);
}

apr_status_t md_acme_order_purge(md_store_t *store, apr_pool_t *p, md_store_group_t group,
                                 const char *md_name)
{
    return md_util_pool_vdo(p_purge, store, p, group, md_name, NULL);
}

/**************************************************************************************************/
/* processing */

typedef struct {
    apr_pool_t *p;
    md_acme_order_t *order;
    md_acme_t *acme;
    const md_t *md;
} order_ctx_t;

apr_status_t md_acme_order_start_challenges(md_acme_order_t *order, md_acme_t *acme, 
                                            apr_array_header_t *challenge_types,
                                            md_store_t *store, const md_t *md, apr_pool_t *p)
{
    apr_status_t rv = APR_SUCCESS;
    md_acme_authz_t *authz;
    const char *url;
    int i;
    
    for (i = 0; i < order->authz_urls->nelts; ++i) {
        url = APR_ARRAY_IDX(order->authz_urls, i, const char*);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, p, "%s: check AUTHZ at %s", md->name, url);
        
        if (APR_SUCCESS != (rv = md_acme_authz_retrieve(acme, p, url, &authz))) {
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, p, "%s: check authz for %s",
                          md->name, authz->domain);
            goto out;
        }

        switch (authz->state) {
            case MD_ACME_AUTHZ_S_VALID:
                break;
                
            case MD_ACME_AUTHZ_S_PENDING:
                rv = md_acme_authz_respond(authz, acme, store, challenge_types, md->pkey_spec, p);
                if (APR_SUCCESS != rv) {
                    goto out;
                }
                md_acme_order_add_challenge_dir(order, authz->dir);
                md_acme_order_save(store, p, MD_SG_STAGING, md->name, order, 0);
                break;
                
            default:
                rv = APR_EINVAL;
                md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, p, "%s: unexpected AUTHZ state %d at %s", 
                              authz->domain, authz->state, url);
             goto out;
        }
    }
out:    
    return rv;
}

static apr_status_t check_challenges(void *baton, int attempt)
{
    order_ctx_t *ctx = baton;
    const char *url;
    md_acme_authz_t *authz;
    apr_status_t rv = APR_SUCCESS;
    int i;
    
    for (i = 0; i < ctx->order->authz_urls->nelts && APR_SUCCESS == rv; ++i) {
        url = APR_ARRAY_IDX(ctx->order->authz_urls, i, const char*);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, ctx->p, "%s: check AUTHZ at %s(%d. attempt)", 
                      ctx->md->name, url, attempt);
        if (APR_SUCCESS == (rv = md_acme_authz_retrieve(ctx->acme, ctx->p, url, &authz))) {
            switch (authz->state) {
                case MD_ACME_AUTHZ_S_VALID:
                    break;
                case MD_ACME_AUTHZ_S_PENDING:
                    rv = APR_EAGAIN;
                    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, ctx->p, 
                                  "%s: status pending at %s", authz->domain, authz->url);
                    break;
                default:
                    rv = APR_EINVAL;
                    md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, ctx->p, 
                                  "%s: unexpected AUTHZ state %d at %s", 
                                  authz->domain, authz->state, authz->url);
                    break;
            }
        }
    }
    return rv;
}

apr_status_t md_acme_order_monitor_authzs(md_acme_order_t *order, md_acme_t *acme, 
                                          const md_t *md, apr_interval_time_t timeout, 
                                          apr_pool_t *p)
{
    order_ctx_t ctx;
    apr_status_t rv;
    
    ctx.p = p;
    ctx.order = order;
    ctx.acme = acme;
    ctx.md = md;
    rv = md_util_try(check_challenges, &ctx, 0, timeout, 0, 0, 1);
    
    md_log_perror(MD_LOG_MARK, MD_LOG_INFO, rv, p, "%s: checked authorizations", md->name);
    return rv;
}

