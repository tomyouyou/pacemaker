/*
 * Copyright 2017-2021 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <glib.h>
#include <regex.h>

#include <crm/crm.h>
#include <crm/lrmd.h>

#include <pacemaker-controld.h>

#if ENABLE_VERSIONED_ATTRS
static regex_t *version_format_regex = NULL;
#endif

static void
ra_param_free(void *param)
{
    if (param) {
        struct ra_param_s *p = (struct ra_param_s *) param;

        if (p->rap_name) {
            free(p->rap_name);
        }
        free(param);
    }
}

static void
metadata_free(void *metadata)
{
    if (metadata) {
        struct ra_metadata_s *md = (struct ra_metadata_s *) metadata;

        if (md->ra_version) {
            free(md->ra_version);
        }
        g_list_free_full(md->ra_params, ra_param_free);
        free(metadata);
    }
}

GHashTable *
metadata_cache_new()
{
    return pcmk__strkey_table(free, metadata_free);
}

void
metadata_cache_free(GHashTable *mdc)
{
    if (mdc) {
        crm_trace("Destroying metadata cache with %d members", g_hash_table_size(mdc));
        g_hash_table_destroy(mdc);
    }
}

void
metadata_cache_reset(GHashTable *mdc)
{
    if (mdc) {
        crm_trace("Resetting metadata cache with %d members",
                  g_hash_table_size(mdc));
        g_hash_table_remove_all(mdc);
    }
}

#if ENABLE_VERSIONED_ATTRS
static gboolean
valid_version_format(const char *version)
{
    if (version == NULL) {
        return FALSE;
    }

    if (version_format_regex == NULL) {
        /* The OCF standard allows free-form versioning, but for our purposes of
         * versioned resource and operation attributes, we constrain it to
         * dot-separated numbers. Agents are still free to use other schemes,
         * but we can't determine attributes based on them.
         */
        const char *regex_string = "^[[:digit:]]+([.][[:digit:]]+)*$";

        version_format_regex = calloc(1, sizeof(regex_t));
        regcomp(version_format_regex, regex_string, REG_EXTENDED | REG_NOSUB);

        /* If our regex doesn't compile, it's a bug on our side, so CRM_CHECK()
         * will give us a core dump to catch it. Pretend the version is OK
         * because we don't want our mistake to break versioned attributes
         * (which should only ever happen in a development branch anyway).
         */
        CRM_CHECK(version_format_regex != NULL, return TRUE);
    }

    return regexec(version_format_regex, version, 0, NULL, 0) == 0;
}
#endif

void
metadata_cache_fini()
{
#if ENABLE_VERSIONED_ATTRS
    if (version_format_regex) {
        regfree(version_format_regex);
        free(version_format_regex);
        version_format_regex = NULL;
    }
#endif
}

#if ENABLE_VERSIONED_ATTRS
static char *
ra_version_from_xml(xmlNode *metadata_xml, const lrmd_rsc_info_t *rsc)
{
    const char *version = crm_element_value(metadata_xml, XML_ATTR_VERSION);

    if (version == NULL) {
        crm_debug("Metadata for %s:%s:%s does not specify a version",
                  rsc->standard, rsc->provider, rsc->type);
        version = PCMK_DEFAULT_AGENT_VERSION;

    } else if (!valid_version_format(version)) {
        crm_notice("%s:%s:%s metadata version has unrecognized format",
                  rsc->standard, rsc->provider, rsc->type);
        version = PCMK_DEFAULT_AGENT_VERSION;

    } else {
        crm_debug("Metadata for %s:%s:%s has version %s",
                  rsc->standard, rsc->provider, rsc->type, version);
    }
    return strdup(version);
}
#endif

static struct ra_param_s *
ra_param_from_xml(xmlNode *param_xml)
{
    const char *param_name = crm_element_value(param_xml, "name");
    const char *value;
    struct ra_param_s *p;

    p = calloc(1, sizeof(struct ra_param_s));
    if (p == NULL) {
        crm_crit("Could not allocate memory for resource metadata");
        return NULL;
    }

    p->rap_name = strdup(param_name);
    if (p->rap_name == NULL) {
        crm_crit("Could not allocate memory for resource metadata");
        free(p);
        return NULL;
    }

    value = crm_element_value(param_xml, "unique");
    if (crm_is_true(value)) {
        controld_set_ra_param_flags(p, ra_param_unique);
    }

    value = crm_element_value(param_xml, "private");
    if (crm_is_true(value)) {
        controld_set_ra_param_flags(p, ra_param_private);
    }
    return p;
}

// Check what version of the OCF standard a resource agent supports
static void
check_ra_ocf_version(struct ra_metadata_s *md, const char *key,
                     xmlNode *version_element)
{
    xmlChar *content = NULL;

    if (version_element != NULL) {
        content = xmlNodeGetContent(version_element);
    }
    if (content == NULL) {
        crm_warn("%s does not advertise OCF version supported", key);
        return;
    }

    if (compare_version((const char *) content, "2") >= 0) {
        crm_warn("%s supports OCF version %s and we don't (agent may not work "
                 "properly with this version of Pacemaker)",
                 key, (const char *) content);
    } else {
        crm_debug("%s advertises support for OCF version %s", key,
                  (const char *) content);
    }
    xmlFree(content);
}

struct ra_metadata_s *
metadata_cache_update(GHashTable *mdc, lrmd_rsc_info_t *rsc,
                      const char *metadata_str)
{
    char *key = NULL;
    xmlNode *metadata = NULL;
    xmlNode *match = NULL;
    struct ra_metadata_s *md = NULL;
    bool any_private_params = false;

    CRM_CHECK(mdc && rsc && metadata_str, return NULL);

    key = crm_generate_ra_key(rsc->standard, rsc->provider, rsc->type);
    if (!key) {
        crm_crit("Could not allocate memory for resource metadata");
        goto err;
    }

    metadata = string2xml(metadata_str);
    if (!metadata) {
        crm_err("Metadata for %s:%s:%s is not valid XML",
                rsc->standard, rsc->provider, rsc->type);
        goto err;
    }

    md = calloc(1, sizeof(struct ra_metadata_s));
    if (md == NULL) {
        crm_crit("Could not allocate memory for resource metadata");
        goto err;
    }

#if ENABLE_VERSIONED_ATTRS
    md->ra_version = ra_version_from_xml(metadata, rsc);
#endif

    if (strcmp(rsc->standard, PCMK_RESOURCE_CLASS_OCF) == 0) {
        check_ra_ocf_version(md, key, first_named_child(metadata, "version"));
    }

    // Check supported actions
    match = first_named_child(metadata, "actions");
    for (match = first_named_child(match, "action"); match != NULL;
         match = crm_next_same_xml(match)) {

        const char *action_name = crm_element_value(match, "name");

        if (pcmk__str_eq(action_name, "reload", pcmk__str_casei)) {
            controld_set_ra_flags(md, key, ra_supports_reload);
            break; // since this is the only action we currently care about
        }
    }

    // Build a parameter list
    match = first_named_child(metadata, "parameters");
    for (match = first_named_child(match, "parameter"); match != NULL;
         match = crm_next_same_xml(match)) {

        const char *param_name = crm_element_value(match, "name");

        if (param_name == NULL) {
            crm_warn("Metadata for %s:%s:%s has parameter without a name",
                     rsc->standard, rsc->provider, rsc->type);
        } else {
            struct ra_param_s *p = ra_param_from_xml(match);

            if (p == NULL) {
                goto err;
            }
            if (pcmk_is_set(p->rap_flags, ra_param_private)) {
                any_private_params = true;
            }
            md->ra_params = g_list_prepend(md->ra_params, p);
        }
    }

    /* Newer resource agents support the "private" parameter attribute to
     * indicate sensitive parameters. For backward compatibility with older
     * agents, implicitly treat a few common names as private when the agent
     * doesn't specify any explicitly.
     */
    if (!any_private_params) {
        for (GList *iter = md->ra_params; iter != NULL; iter = iter->next) {
            struct ra_param_s *p = iter->data;

            if (pcmk__str_any_of(p->rap_name, "password", "passwd", "user",
                                 NULL)) {
                controld_set_ra_param_flags(p, ra_param_private);
            }
        }
    }

    g_hash_table_replace(mdc, key, md);
    free_xml(metadata);
    return md;

err:
    free(key);
    free_xml(metadata);
    metadata_free(md);
    return NULL;
}

struct ra_metadata_s *
metadata_cache_get(GHashTable *mdc, lrmd_rsc_info_t *rsc)
{
    char *key = NULL;
    struct ra_metadata_s *metadata = NULL;

    CRM_CHECK(mdc && rsc, return NULL);
    key = crm_generate_ra_key(rsc->standard, rsc->provider, rsc->type);
    if (key) {
        metadata = g_hash_table_lookup(mdc, key);
        free(key);
    }
    return metadata;
}
