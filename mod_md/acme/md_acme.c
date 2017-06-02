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

#include "md_acme.h"
#include "md_acme_acct.h"
#include "../md.h"
#include "../md_crypt.h"
#include "../md_json.h"
#include "../md_jws.h"
#include "../md_http.h"
#include "../md_log.h"
#include "../md_reg.h"
#include "../md_util.h"

#define MD_DIRNAME_ACCOUNTS "accounts"

typedef struct acme_problem_status_t acme_problem_status_t;

struct acme_problem_status_t {
    const char *type;
    apr_status_t rv;
};

static acme_problem_status_t Problems[] = {
    { "acme:error:badCSR",                       APR_EINVAL },
    { "acme:error:badNonce",                     APR_EGENERAL },
    { "acme:error:badSignatureAlgorithm",        APR_EINVAL },
    { "acme:error:invalidContact",               APR_BADARG },
    { "acme:error:unsupportedContact",           APR_EGENERAL },
    { "acme:error:malformed",                    APR_EINVAL },
    { "acme:error:rateLimited",                  APR_BADARG },
    { "acme:error:rejectedIdentifier",           APR_BADARG },
    { "acme:error:serverInternal",               APR_EGENERAL },
    { "acme:error:unauthorized",                 APR_EACCES },
    { "acme:error:unsupportedIdentifier",        APR_BADARG },
    { "acme:error:userActionRequired",           APR_EAGAIN },
    { "acme:error:badRevocationReason",          APR_EINVAL },
    { "acme:error:caa",                          APR_EGENERAL },
    { "acme:error:dns",                          APR_EGENERAL },
    { "acme:error:connection",                   APR_EGENERAL },
    { "acme:error:tls",                          APR_EGENERAL },
    { "acme:error:incorrectResponse",            APR_EGENERAL },
};

static apr_status_t problem_status_get(const char *type) {
    int i;

    if (strstr(type, "urn:ietf:params:") == type) {
        type += strlen("urn:ietf:params:");
    }
    else if (strstr(type, "urn:") == type) {
        type += strlen("urn:");
    }
     
    for(i = 0; i < (sizeof(Problems)/sizeof(Problems[0])); ++i) {
        if (!apr_strnatcasecmp(type, Problems[i].type)) {
            return Problems[i].rv;
        }
    }
    return APR_EGENERAL;
}

apr_status_t md_acme_init(apr_pool_t *p)
{
    return md_crypt_init(p);
}

apr_status_t md_acme_create(md_acme_t **pacme, apr_pool_t *p, const char *url,
                            struct md_store_t *store)
{
    md_acme_t *acme;
    
    if (!url) {
        md_log_perror(MD_LOG_MARK, MD_LOG_ERR, 0, p, "create ACME without url");
        return APR_EINVAL;
    }
    
    acme = apr_pcalloc(p, sizeof(*acme));
    acme->url = url;
    acme->pool = p;
    acme->store = store;
    acme->pkey_bits = 4096;
    
    *pacme = acme;
    return md_http_create(&acme->http, acme->pool);
}

apr_status_t md_acme_setup(md_acme_t *acme)
{
    apr_status_t rv;
    md_json_t *json;
    
    assert(acme->url);
    md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, acme->pool, "get directory from %s", acme->url);
    
    rv = md_json_http_get(&json, acme->pool, acme->http, acme->url);
    if (APR_SUCCESS == rv) {
        acme->new_authz = md_json_gets(json, "new-authz", NULL);
        acme->new_cert = md_json_gets(json, "new-cert", NULL);
        acme->new_reg = md_json_gets(json, "new-reg", NULL);
        acme->revoke_cert = md_json_gets(json, "revoke-cert", NULL);
        if (acme->new_authz && acme->new_cert && acme->new_reg && acme->revoke_cert) {
            return APR_SUCCESS;
        }
        rv = APR_EINVAL;
    }
    return rv;
}

/**************************************************************************************************/
/* acme requests */

static void req_update_nonce(md_acme_req_t *req)
{
    if (req->resp_hdrs) {
        const char *nonce = apr_table_get(req->resp_hdrs, "Replay-Nonce");
        if (nonce) {
            req->acme->nonce = nonce;
        }
    }
}

static apr_status_t http_update_nonce(const md_http_response_t *res)
{
    if (res->headers) {
        const char *nonce = apr_table_get(res->headers, "Replay-Nonce");
        if (nonce) {
            md_acme_t *acme = res->req->baton;
            acme->nonce = nonce;
        }
    }
    return res->rv;
}

static apr_status_t md_acme_new_nonce(md_acme_t *acme)
{
    apr_status_t rv;
    long id;
    
    rv = md_http_HEAD(acme->http, acme->new_reg, NULL, http_update_nonce, acme, &id);
    md_http_await(acme->http, id);
    return rv;
}

static md_acme_req_t *md_acme_req_create(md_acme_t *acme, const char *url)
{
    apr_pool_t *pool;
    md_acme_req_t *req;
    apr_status_t rv;
    
    rv = apr_pool_create(&pool, acme->pool);
    if (rv != APR_SUCCESS) {
        return NULL;
    }
    
    req = apr_pcalloc(pool, sizeof(*req));
    if (!req) {
        apr_pool_destroy(pool);
        return NULL;
    }
        
    req->acme = acme;
    req->pool = pool;
    req->url = url;
    req->prot_hdrs = apr_table_make(pool, 5);
    if (!req->prot_hdrs) {
        apr_pool_destroy(pool);
        return NULL;
    }
    return req;
}
 
apr_status_t md_acme_req_body_init(md_acme_req_t *req, md_json_t *jpayload, md_pkey_t *key)
{
    const char *payload = md_json_writep(jpayload, MD_JSON_FMT_COMPACT, req->pool);
    size_t payload_len = strlen(payload);
    
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, 0, req->pool, 
                  "acct payload(len=%d): %s", payload_len, payload);
    return md_jws_sign(&req->req_json, req->pool, payload, payload_len,
                       req->prot_hdrs, key, NULL);
} 


static apr_status_t inspect_problem(md_acme_req_t *req, const md_http_response_t *res)
{
    const char *ctype;
    md_json_t *problem;
    
    ctype = apr_table_get(req->resp_hdrs, "content-type");
    if (ctype && !strcmp(ctype, "application/problem+json")) {
        /* RFC 7807 */
        md_json_read_http(&problem, req->pool, res);
        if (problem) {
            const char *ptype, *pdetail;
            
            req->resp_json = problem;
            ptype = md_json_gets(problem, "type", NULL); 
            pdetail = md_json_gets(problem, "detail", NULL);
            req->rv = problem_status_get(ptype);
             
            md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, req->rv, req->pool,
                          "acme problem %s: %s", ptype, pdetail);
            return req->rv;
        }
    }
    md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, 0, req->pool,
                  "acme problem unknonw: http status %d", res->status);
    return APR_EGENERAL;
}

static apr_status_t md_acme_req_done(md_acme_req_t *req)
{
    apr_status_t rv = req->rv;
    if (req->pool) {
        apr_pool_destroy(req->pool);
    }
    return rv;
}

static apr_status_t on_response(const md_http_response_t *res)
{
    md_acme_req_t *req = res->req->baton;
    apr_status_t rv = res->rv;
    
    if (rv != APR_SUCCESS) {
        md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, rv, res->req->pool, "req failed");
        return rv;
    }
    
    req->resp_hdrs = apr_table_clone(req->pool, res->headers);
    req_update_nonce(req);
    
    /* TODO: Redirect Handling? */
    if (res->status >= 200 && res->status < 300) {
        rv = md_json_read_http(&req->resp_json, req->pool, res);
        if (rv != APR_SUCCESS) {
                md_log_perror(MD_LOG_MARK, MD_LOG_WARNING, rv, req->pool, 
                              "unable to parse JSON response body");
                return APR_EINVAL;
        }
        
        if (md_log_is_level(req->pool, MD_LOG_TRACE2)) {
            md_log_perror(MD_LOG_MARK, MD_LOG_TRACE2, rv, req->pool,
                          "acme response: %s", md_json_writep(req->resp_json, 
                                                              MD_JSON_FMT_INDENT, req->pool));
        }
    
        if (req->on_success) {
            req->rv = req->on_success(req->acme, req->resp_hdrs, req->resp_json, req->baton);
        }
    }
    else {
        req->rv = rv;
        rv = inspect_problem(req, res);
    }
    
    md_acme_req_done(req);
    return rv;
}

static apr_status_t md_acme_req_send(md_acme_req_t *req)
{
    apr_status_t rv;
    md_acme_t *acme = req->acme;

    assert(acme->url);
    
    if (!acme->new_authz) {
        if (APR_SUCCESS != (rv = md_acme_setup(acme))) {
            return rv;
        }
    }
    if (!acme->nonce) {
        if (APR_SUCCESS != (rv = md_acme_new_nonce(acme))) {
            return rv;
        }
    }
    
    apr_table_set(req->prot_hdrs, "nonce", acme->nonce);
    acme->nonce = NULL;

    rv = req->on_init(req, req->baton);
    
    if (rv == APR_SUCCESS) {
        long id;
        const char *body = NULL;
    
        if (req->req_json) {
            body = md_json_writep(req->req_json, MD_JSON_FMT_INDENT, req->pool);
        }
        
        if (body && md_log_is_level(req->pool, MD_LOG_TRACE2)) {
            md_log_perror(MD_LOG_MARK, MD_LOG_TRACE2, 0, req->pool, 
                          "req: POST %s, body:\n%s", req->url, body);
        }
        else {
            md_log_perror(MD_LOG_MARK, MD_LOG_DEBUG, 0, req->pool, 
                          "req: POST %s\n", req->url);
        }
        rv = md_http_POSTd(req->acme->http, req->url, NULL, "application/json",  
                               body, body? strlen(body) : 0, on_response, req, &id);
        req = NULL;
        md_http_await(acme->http, id);
    }

    if (req) {
        md_acme_req_done(req);
    }
    return rv;
}

apr_status_t md_acme_req_do(md_acme_t *acme, const char *url,
                            md_acme_req_init_cb *on_init,
                            md_acme_req_success_cb *on_success,
                            void *baton)
{
    md_acme_req_t *req;
    
    md_log_perror(MD_LOG_MARK, MD_LOG_TRACE1, 0, acme->pool, "add acme req: %s", url);
    req = md_acme_req_create(acme, url);
    req->on_init = on_init;
    req->on_success = on_success;
    req->baton = baton;
    
    return md_acme_req_send(req);
}

/**************************************************************************************************/
/* protocol drivers */

static apr_status_t acme_driver_init(md_proto_driver_t *driver)
{
    apr_status_t rv;
    md_acme_t *acme;
    
    /* Find out where we're at with this managed domain */
    if (APR_SUCCESS == (rv = md_acme_create(&acme, driver->p, driver->md->ca_url, driver->store))) {
        /* 1.  */
        /* 2. Get an account for the server for this MD */
        /* 2.0 If MD has no account, find a local account for server, store at MD */ 
        /* 2.1 verify that the MD account is valid */
        /* 2.1.1 if account valid, goto 3 */
        /* 2.1.3 disable local account, remove from MD, goto 2.0 */
        /* 2.2 No local account exists, create a new one */
        /* 2.2.1 Make a 'newreg' at ACME server */
        /* 2.2.2 Store account info locally and at MD */
        
        /* 3. Check if Terms-of-Service for MD account were accepted */
        /* 3.1 Update MD account with accepted TOS url, if necessary */
        /* 3.2 If rejected, fail */
        
        /* 4 For each domain in MD: AUTHZ setup */
        /* 4.1 If an AUTHZ resource is known, check if it is still valid */
        /* 4.2 If known AUTHZ resource is not valid, remove, goto 4.1.1 */
        /* 4.3 If no AUTHZ available, create a new one for the domain, store it */
        
        /* 5 For each domain in MD: Challenge Response Setup */
        /* 5.0 GET the AUTHZ resource with and local challenge data */
        /* 5.0.1 If complete and matches challenge, continue with next domain */
        /* 5.1 If "http-01" challenge is an option and port 80 is served: */
        /* 5.1.1 Calculate the challenge data. */
        /* 5.1.2 Store data in HTTP Challenge Directory */
        /* 5.1.3 continue with next domain at 5 */
        /* 5.2 If "tls-sni-02" is an option and port 443 is served: */
        /* 5.2.1 Calculate challenge data */
        /* 5.2.2 Create a self-signed certificate with the correct alt names */
        /* 5.2.3 Store the cert and its key in the TLS-SNI Challenge Directory */
        /* 5.2.4 continue with next domain at 5 */
        /* 5.3 No suitable challenge found, fail */
        
        /* 6 For each domain in MD: Challenge Response */
        /* 6.1 Lookup challenge data and response data */
        /* 6.2 POST challenge response to ACME server */
        
        /* 7 For each domain in MD: Validation Check */
        /* 7.1 GET the challenge resource */
        /* 7.1.1 if "status" is "pending", continue at 7 */
        /* 7.1.2 store status and "expires" at MD domain, continue at 7 */
        
        /* 8 If any domain is MD is "pending": */
        /* 8.1 if max waiting time has been reached, fail */
        /* 8.2 Pause for n seconds, goto 7 */
        
        /* All domains in MD have been validated */
        
        /* Create a CSR for the MD with all domains */
        /* POST the CSR to the "new-order" resource */
        /* On 201 answer, check Location or body content type for cert */
        /* parse cert into PEM, retrieve CA Information Access, download chain */
        /* store cert and chain */
        /* Update MD expiration date */
    }
    return rv;
}

static apr_status_t acme_driver_run(md_proto_driver_t *driver)
{
    return APR_ENOTIMPL;
}

static md_proto_t ACME_PROTO = {
    MD_PROTO_ACME, acme_driver_init, acme_driver_run
};
 
apr_status_t md_acme_protos_add(apr_hash_t *protos, apr_pool_t *p)
{
    apr_hash_set(protos, MD_PROTO_ACME, sizeof(MD_PROTO_ACME)-1, &ACME_PROTO);
    return APR_SUCCESS;
}