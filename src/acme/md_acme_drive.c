/* Copyright 2017 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdlib.h>

#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_buckets.h>
#include <apr_hash.h>
#include <apr_uri.h>

#include "../md.h"
#include "../md_crypt.h"
#include "../md_json.h"
#include "../md_jws.h"
#include "../md_http.h"
#include "../md_log.h"
#include "../md_reg.h"
#include "../md_store.h"
#include "../md_util.h"

#include "md_acme.h"
#include "md_acme_acct.h"
#include "md_acme_authz.h"
#include "md_acme_drive.h"

typedef struct {
    md_proto_driver_t *driver;
    
    const char *phase;
    
    md_acme_t *acme;
    md_acme_acct_t *acct;
    md_t *md;
    
    unsigned can_http_01 : 1;
    unsigned can_tls_sni_01 : 1;
    md_acme_authz_set_t *authz_set;
    apr_interval_time_t authz_monitor_timeout;
    
    const char *csr_der_64;
    apr_interval_time_t cert_poll_timeout;
    md_cert_t *cert;
    
} md_acme_driver_t;

/**************************************************************************************************/
/* account setup */

static apr_status_t ad_acct_validate(md_proto_driver_t *d, md_acme_acct_t **pacct)
{
    md_acme_driver_t *ad = d->baton;
    md_acme_acct_t *acct = *pacct;
    apr_status_t rv;
    
    if (APR_SUCCESS != (rv = md_acme_acct_validate(ad->acme, *pacct))) {
        if (APR_ENOENT == rv || APR_EACCES == rv) {
            *pacct = NULL;
            rv = md_acme_acct_disable(acct);
        }
    }
    return rv;
}

static apr_status_t ad_set_acct(md_proto_driver_t *d) 
{
    md_acme_driver_t *ad = d->baton;
    md_t *md = ad->md;
    md_acme_acct_t *acct = NULL;
    apr_status_t rv = APR_SUCCESS;

    ad->phase = "choose account";
    ad->acct = NULL;
    
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: finding account",
                  d->proto->protocol);
    
    /* Get an account for the ACME server for this MD */
    if (ad->md->ca_account) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: checking previous account %s",
                      d->proto->protocol, md->ca_account);
        if (APR_SUCCESS == (rv = md_acme_acct_load(&acct, d->store, md->ca_account, d->p))) {
            rv = ad_acct_validate(d, &acct);
        }
        else if (APR_ENOENT == rv) {
            rv = APR_SUCCESS;
        }
    }
    
    /* If MD has no account, find a local account for server, store at MD */ 
    if (APR_SUCCESS == rv && NULL == acct) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: looking at existing accounts",
                      d->proto->protocol);
        while (NULL == acct 
               && APR_SUCCESS == (rv = md_acme_acct_find(&acct, d->store, ad->acme, d->p))) {
            rv = ad_acct_validate(d, &acct);
        }
        if (!acct && APR_ENOENT == rv) {
            rv = APR_SUCCESS;
        }
    }
    
    if (APR_SUCCESS == rv && NULL == acct) {
        /* 2.2 No local account exists, create a new one */
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: creating new account", 
                      d->proto->protocol);

        if (!ad->md->contacts || apr_is_empty_array(md->contacts)) {
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, APR_EINVAL, d->p, 
                "no contact information for md %s", md->name);            
            return APR_EINVAL;
        }
        
        rv = md_acme_register(&acct, d->store, ad->acme, md->contacts, md->ca_agreement);
        if (APR_SUCCESS != rv) {
            md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, "register new account");
        }
    }
    
    if (APR_SUCCESS == rv) {
        ad->acct = acct;
        /* Persist the account chosen at the md so we use the same on future runs */
        if (!md->ca_account || strcmp(md->ca_account, acct->id)) {
            md->ca_account = acct->id;
            rv = md_reg_update(d->reg, md->name, md, MD_UPD_CA_ACCOUNT);
        }
    }

    return rv;
}

/**************************************************************************************************/
/* authz/challenge setup */

/**
 * Pre-Req: we have an account for the ACME server that has accepted the current license agreement
 * For each domain in MD: 
 * - check if there already is a valid AUTHZ resource
 * - if ot, create an AUTHZ resource with challenge data 
 */
static apr_status_t ad_setup_authz(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;
    md_t *md = ad->md;
    md_acme_authz_t *authz;
    int i, changed;
    
    assert(ad->md);
    assert(ad->acme);
    assert(ad->acct);

    ad->phase = "check authz";
    
    /* For each domain in MD: AUTHZ setup
     * if an AUTHZ resource is known, check if it is still valid
     * if known AUTHZ resource is not valid, remove, goto 4.1.1
     * if no AUTHZ available, create a new one for the domain, store it
     */
    rv = md_acme_authz_set_load(d->store, md->name, &ad->authz_set, d->p);
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: loading authz data", md->name);
    if (APR_ENOENT == rv) {
        ad->authz_set = md_acme_authz_set_create(d->p, ad->acct->id);
        rv = APR_SUCCESS;
    }
    
    for (i = 0; i < md->domains->nelts && APR_SUCCESS == rv; ++i) {
        const char *domain = APR_ARRAY_IDX(md->domains, i, const char *);
        changed = 0;
        authz = md_acme_authz_set_get(ad->authz_set, domain);
        if (authz) {
            /* check valid */
            rv = md_acme_authz_update(authz, ad->acme, ad->acct, d->p);
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: updated authz for %s", 
                          md->name, domain);
            if (APR_SUCCESS != rv) {
                md_acme_authz_set_remove(ad->authz_set, domain);
                authz = NULL;
                changed = 1;
            }
        }
        if (!authz) {
            /* create new one */
            rv = md_acme_authz_register(&authz, ad->acme, domain, ad->acct, d->p);
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: created authz for %s", 
                          md->name, domain);
            if (APR_SUCCESS == rv) {
                rv = md_acme_authz_set_add(ad->authz_set, authz);
                changed = 1;
            }
        }
        if (APR_SUCCESS == rv && changed) {
            rv = md_acme_authz_set_save(d->store, md->name, ad->authz_set, 0);
        }
    }
    
    return rv;
}

/**
 * Pre-Req: all domains have a AUTHZ resources at the ACME server
 * For each domain in MD: 
 * - if AUTHZ resource is 'valid' -> continue
 * - if AUTHZ resource is 'pending':
 *   - find preferred challenge choice
 *   - calculate challenge data for httpd to find
 *   - POST challenge start to ACME server
 * For each domain in MD where AUTHZ is 'pending', until overall timeout: 
 *   - wait a certain time, check status again
 * If not all AUTHZ are valid, fail
 */
static apr_status_t ad_start_challenges(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv = APR_SUCCESS;
    md_acme_authz_t *authz;
    int i;
    
    assert(ad->md);
    assert(ad->acme);
    assert(ad->acct);
    assert(ad->authz_set);
    assert(ad->authz_set->authzs->nelts == ad->md->domains->nelts);

    ad->phase = "start challenges";

    for (i = 0; i < ad->authz_set->authzs->nelts && APR_SUCCESS == rv; ++i) {
        authz = APR_ARRAY_IDX(ad->authz_set->authzs, i, md_acme_authz_t*);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: check AUTHZ for %s", 
                      ad->md->name, authz->domain);
        if (APR_SUCCESS == (rv = md_acme_authz_update(authz, ad->acme, ad->acct, d->p))) {
            switch (authz->state) {
                case MD_ACME_AUTHZ_S_VALID:
                    break;
                case MD_ACME_AUTHZ_S_PENDING:
                    rv = md_acme_authz_respond(authz, ad->acme, ad->acct, d->store,
                                               ad->can_http_01, ad->can_tls_sni_01, d->p);
                    break;
                default:
                    rv = APR_EINVAL;
                    md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                                  "%s: unexpected AUTHZ state %d at %s", 
                                  authz->domain, authz->state, authz->location);
                    break;
            }
        }
    }
    
    return rv;
}

static apr_status_t check_challenges(void *baton, int attemmpt)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    md_acme_authz_t *authz;
    apr_status_t rv = APR_SUCCESS;
    int i;
    
    for (i = 0; i < ad->authz_set->authzs->nelts && APR_SUCCESS == rv; ++i) {
        authz = APR_ARRAY_IDX(ad->authz_set->authzs, i, md_acme_authz_t*);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: check AUTHZ for %s", 
                      ad->md->name, authz->domain);
        if (APR_SUCCESS == (rv = md_acme_authz_update(authz, ad->acme, ad->acct, d->p))) {
            switch (authz->state) {
                case MD_ACME_AUTHZ_S_VALID:
                    break;
                case MD_ACME_AUTHZ_S_PENDING:
                    rv = APR_EAGAIN;
                    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, 
                                  "%s: status pending at %s", authz->domain, authz->location);
                    break;
                default:
                    rv = APR_EINVAL;
                    md_log_perror(MD_LOG_MARK, MD_LOG_ERR, rv, d->p, 
                                  "%s: unexpected AUTHZ state %d at %s", 
                                  authz->domain, authz->state, authz->location);
                    break;
            }
        }
    }
    return rv;
}

static apr_status_t ad_monitor_challenges(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;
    
    assert(ad->md);
    assert(ad->acme);
    assert(ad->acct);
    assert(ad->authz_set);
    assert(ad->authz_set->authzs->nelts == ad->md->domains->nelts);

    ad->phase = "monitor challenges";
    rv = md_util_try(check_challenges, d, 0, ad->authz_monitor_timeout, 0, 0, 1);
    
    md_log_perror(MD_LOG_MARK, MD_LOG_INFO, rv, d->p, 
                  "%s: checked all domain authorizations", ad->md->name);
    return rv;
}

/**************************************************************************************************/
/* poll cert */


static apr_status_t on_got_cert(md_acme_t *acme, const md_http_response_t *res, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv = APR_SUCCESS;
    
    if (APR_SUCCESS == (rv = md_cert_read_http(&ad->cert, d->p, res))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "cert parsed");
    }
    else if (APR_STATUS_IS_ENOENT(rv)) {
        rv = APR_EAGAIN;
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, 
                      "cert not in response from %s", ad->md->cert_url);
    }
    
    return rv;
}

static apr_status_t get_cert(void *baton, int attempt)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    
    return md_acme_GET(ad->acme, ad->md->cert_url, NULL, NULL, on_got_cert, d);
}

static apr_status_t ad_cert_poll(md_proto_driver_t *d, int only_once)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;
    
    assert(ad->md);
    assert(ad->acme);
    assert(ad->md->cert_url);
    
    ad->phase = "poll certificate";
    if (only_once) {
        rv = get_cert(d, 0);
    }
    else {
        rv = md_util_try(get_cert, d, 1, ad->cert_poll_timeout, 0, 0, 1);
    }
    
    md_log_perror(MD_LOG_MARK, MD_LOG_INFO, 0, d->p, "poll for cert at %s", ad->md->cert_url);
    return rv;
}

/**************************************************************************************************/
/* cert setup */

static apr_status_t on_init_csr_req(md_acme_req_t *req, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    md_json_t *jpayload;

    jpayload = md_json_create(req->pool);
    md_json_sets("new-cert", jpayload, MD_KEY_RESOURCE, NULL);
    md_json_sets(ad->csr_der_64, jpayload, MD_KEY_CSR, NULL);
    
    return md_acme_req_body_init(req, jpayload, ad->acct->key);
} 

static apr_status_t csr_req(md_acme_t *acme, const md_http_response_t *res, void *baton)
{
    md_proto_driver_t *d = baton;
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv = APR_SUCCESS;
    
    ad->md->cert_url = apr_table_get(res->headers, "location");
    if (!ad->md->cert_url) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, APR_EINVAL, d->p, 
                      "cert created without giving its location header");
        return APR_EINVAL;
    }
    if (APR_SUCCESS != (rv = md_reg_update(d->reg, ad->md->name, ad->md, MD_UPD_CERT_URL))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, APR_EINVAL, d->p, 
                      "%s: saving cert url %s", ad->md->name, ad->md->cert_url);
        return rv;
    }
    
    /* Check if it already was sent with this response */
    if (APR_SUCCESS == (rv = md_cert_read_http(&ad->cert, d->p, res))) {
        rv = md_store_save(d->store, MD_SG_DOMAINS, ad->md->name, MD_FN_CERT, 
                           MD_SV_CERT, ad->cert, 0);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "cert parsed and saved");
    }
    else if (APR_STATUS_IS_ENOENT(rv)) {
        rv = APR_SUCCESS;
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, 
                      "cert not in response, need to poll %s", ad->md->cert_url);
    }
    
    return rv;
}

/**
 * Pre-Req: all domains have been validated by the ACME server, e.g. all have AUTHZ
 * resources that have status 'valid'
 * - Setup private key, if not already there
 * - Generate a CSR with org, contact, etc
 * - Optionally enable must-staple OCSP extension
 * - Submit CSR, expect 201 with location
 * - POLL location for certificate
 * - store certificate
 * - retrieve cert chain information from cert
 * - GET cert chain
 * - store cert chain
 */
static apr_status_t ad_setup_certificate(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    md_pkey_t *pkey;
    apr_status_t rv;

    ad->phase = "setup cert pkey";
    
    rv = md_pkey_load(d->store, ad->md->name, &pkey, d->p);
    if (APR_STATUS_IS_ENOENT(rv)) {
        if (APR_SUCCESS == (rv = md_pkey_gen_rsa(&pkey, d->p, ad->acme->pkey_bits))) {
            rv = md_pkey_save(d->store, ad->md->name, pkey, 1);
        }
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: generate pkey", ad->md->name);
    }

    if (APR_SUCCESS == rv) {
        ad->phase = "setup csr";
        rv = md_cert_req_create(&ad->csr_der_64, ad->md, pkey, d->p);
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: create CSR", ad->md->name);
    }

    if (APR_SUCCESS == rv) {
        ad->phase = "submit csr";
        rv = md_acme_POST(ad->acme, ad->acme->new_cert, on_init_csr_req, NULL, csr_req, d);
    }

    if (APR_SUCCESS == rv) {
        if (!ad->cert) {
            rv = ad_cert_poll(d, 0);
        }
    }
    return rv;
}

/**************************************************************************************************/
/* ACME driving */

static apr_status_t acme_driver_init(md_proto_driver_t *d)
{
    md_acme_driver_t *ad;
    
    ad = apr_pcalloc(d->p, sizeof(*ad));
    
    d->baton = ad;
    ad->driver = d;
    ad->md = md_copy(d->p, d->md);
    
    ad->authz_monitor_timeout = apr_time_from_sec(30);
    ad->cert_poll_timeout = apr_time_from_sec(30);

    /* TODO: which challenge types do we support? 
     * Need to know if the server listens to the right ports */
    ad->can_http_01 = 1;
    
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, d->p, "%s: driving %s", 
                  ad->md->name, d->proto->protocol);
    
    /* Find out where we're at with this managed domain */
    return md_acme_create(&ad->acme, d->p, ad->md->ca_url, d->store);
}

static apr_status_t acme_drive_cert(md_proto_driver_t *d)
{
    md_acme_driver_t *ad = d->baton;
    apr_status_t rv;

    if (ad->cert) {
        return APR_SUCCESS;
    }

    ad->phase = "get certificate";

    /* Chose (or create) and ACME account to use */
    rv = ad_set_acct(d);
    
    /* Check that the account agreed to the terms-of-service, otherwise
     * requests for new authorizations are denied. ToS may change during the
     * lifetime of an account */
    if (APR_SUCCESS == rv) {
        ad->phase = "check agreement";
        rv = md_acme_acct_check_agreement(ad->acme, ad->acct, ad->md->ca_agreement);
    }
    
    /* If we know a cert's location, try to get it. Previous download might
     * have failed. If server 404 it, we clear our memory of it. */
    if (APR_SUCCESS == rv && ad->md->cert_url) {
        rv = ad_cert_poll(d, 1);
        if (APR_STATUS_IS_ENOENT(rv)) {
            /* Server reports to know nothing about it. */
            ad->md->cert_url = NULL;
            rv = md_reg_update(d->reg, ad->md->name, ad->md, MD_UPD_CERT_URL);
        }
    }
    
    if (APR_SUCCESS == rv
        && APR_SUCCESS == (rv = ad_setup_authz(d))
        && APR_SUCCESS == (rv = ad_start_challenges(d))
        && APR_SUCCESS == (rv = ad_monitor_challenges(d))
        && APR_SUCCESS == (rv = ad_setup_certificate(d))) {
        md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, d->p, "%s: certificate obtained", 
                      ad->md->name);
    }
    return rv;
}

static apr_status_t acme_driver_run(md_proto_driver_t *d)
{
    apr_status_t rv = APR_ENOTIMPL;
    md_acme_driver_t *ad = d->baton;

    assert(ad->md);
    assert(ad->acme);

    ad->phase = "ACME setup";
    rv = md_acme_setup(ad->acme);
    
    if (APR_SUCCESS == rv) {
        rv = acme_drive_cert(d);
    }
    /* Update MD expiration date */
    if (APR_SUCCESS == rv) {
        ad->phase = "completed";
    }
        
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, rv, d->p, "%s: drive %s, %s", 
                  ad->md->name, d->proto->protocol, ad->phase);
    return rv;
}

static md_proto_t ACME_PROTO = {
    MD_PROTO_ACME, acme_driver_init, acme_driver_run
};
 
apr_status_t md_acme_protos_add(apr_hash_t *protos, apr_pool_t *p)
{
    apr_hash_set(protos, MD_PROTO_ACME, sizeof(MD_PROTO_ACME)-1, &ACME_PROTO);
    return APR_SUCCESS;
}
