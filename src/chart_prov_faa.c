/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
*/
/*
 * Copyright 2018 Saso Kiselkov. All rights reserved.
 */

#include <errno.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>

#if	!IBM
#include <sys/stat.h>
#endif	/* !IBM */

#include "acfutils/helpers.h"
#include "acfutils/thread.h"
#include "chart_prov_faa.h"
#include "chart_prov_common.h"

#define	SERVER_NAME	"https://aeronav.faa.gov"
#define	INDEX_URL	SERVER_NAME "/d-tpp/%d/xml_data/d-TPP_Metafile.xml"
#define	CHART_URL	SERVER_NAME "/d-tpp/%d/%s"

static char *
mk_index_path(chartdb_t *cdb)
{
	char airac_nr[8];

	snprintf(airac_nr, sizeof (airac_nr), "%d", cdb->airac);
	return (mkpathname(cdb->path, cdb->prov_name, airac_nr,
	    "d-TPP_Metafile.xml", NULL));
}

static bool_t
update_index(chartdb_t *cdb)
{
	char url[128];
	char *index_path = mk_index_path(cdb);
	bool_t result = B_FALSE;

	snprintf(url, sizeof (url), INDEX_URL, cdb->airac);
	result = chart_download(cdb, url, index_path,
	    "Error downloading chart index", NULL);
	if (!result && file_exists(index_path, NULL)) {
		logMsg("WARNING: failed to contact FAA servers to refresh "
		    "the chart index. This means that downloading new FAA "
		    "charts will most likely not be possible. I will still "
		    "make any locally cached charts available to you.");
		result = B_TRUE;
	}

	free(index_path);

	return (result);
}

static void
load_record(chart_arpt_t *arpt, const xmlNode *rec)
{
	chart_t *chart = calloc(1, sizeof (*chart));

	for (const xmlNode *node = rec->children; node != NULL;
	    node = node->next) {
		const char *content;

		if (node->children == NULL || node->name == NULL)
			continue;
		content = (char *)node->children[0].content;
		if (strcmp((char *)node->name, "chart_name") == 0) {
			free(chart->name);
			chart->name = strdup(content);
		} else if (strcmp((char *)node->name, "faanfd18") == 0) {
			free(chart->codename);
			chart->codename = strdup(content);
		} else if (strcmp((char *)node->name, "pdf_name") == 0) {
			/*
			 * "DELETED_JOB.PDF" in the PDF filename means that
			 * the chart no longer exists, so get rid of it.
			 */
			if (strcmp(content, "DELETED_JOB.PDF") == 0) {
				chart->type = CHART_TYPE_UNKNOWN;
				break;
			}
			free(chart->filename);
			chart->filename = strdup(content);
		} else if (strcmp((char *)node->name, "chart_code") == 0) {
			if (strcmp(content, "APD") == 0)
				chart->type = CHART_TYPE_APD;
			else if (strcmp(content, "IAP") == 0)
				chart->type = CHART_TYPE_IAP;
			else if (strcmp(content, "DP") == 0)
				chart->type = CHART_TYPE_DP;
			else if (strcmp(content, "ODP") == 0)
				chart->type = CHART_TYPE_ODP;
			else if (strcmp(content, "STAR") == 0)
				chart->type = CHART_TYPE_STAR;
			else if (strcmp(content, "MIN") == 0)
				chart->type = CHART_TYPE_MIN;
			else
				chart->type = CHART_TYPE_UNKNOWN;
		}
	}

	if (chart->name[0] == '\0' || chart->type == CHART_TYPE_UNKNOWN ||
	    !chartdb_add_chart(arpt, chart)) {
		free(chart->name);
		free(chart->codename);
		free(chart->filename);
		free(chart);
	}
}

static void
load_airport(chartdb_t *cdb, const xmlNode *arpt_node, const char *city_id,
    const char *state_id)
{
	char *icao_ident = (char *)xmlGetProp(arpt_node,
	    (xmlChar *)"icao_ident");
	char *apt_ident = (char *)xmlGetProp(arpt_node, (xmlChar *)"apt_ident");
	char *apt_name = (char *)xmlGetProp(arpt_node, (xmlChar *)"ID");
	chart_arpt_t *arpt;
	char icao[8];

	if (icao_ident == NULL || apt_ident == NULL || apt_name == NULL) {
		/* Malformed file */
		goto out;
	}
	if (is_valid_icao_code(icao_ident)) {
		/* Normal ICAO identifier present, use it */
		strlcpy(icao, icao_ident, sizeof (icao));
	} else if (strlen(apt_ident) > 0) {
		/*
		 * Local non-ICAO identifier, convert it into a pseudo-ICAO
		 * identifier.
		 */
		snprintf(icao, sizeof (icao), "K%s", apt_ident);
	} else {
		/* No valid ID present, skip the airport */
		goto out;
	}
	arpt = chartdb_add_arpt(cdb, icao, apt_name, city_id, state_id);

	for (const xmlNode *rec = arpt_node->children; rec != NULL;
	    rec = rec->next) {
		if (rec->name == NULL ||
		    strcmp((char *)rec->name, "record") != 0)
			continue;
		load_record(arpt, rec);
	}
out:
	if (icao_ident != NULL)
		xmlFree(icao_ident);
	if (apt_ident != NULL)
		xmlFree(apt_ident);
	if (apt_name != NULL)
		xmlFree(apt_name);
}

static void
load_city(chartdb_t *cdb, const xmlNode *city_node, const char *state_id)
{
	char *city_id = (char *)xmlGetProp(city_node, (xmlChar *)"ID");

	if (city_id == NULL)
		return;

	for (const xmlNode *arpt_node = city_node->children; arpt_node != NULL;
	    arpt_node = arpt_node->next) {
		if (arpt_node->name == NULL ||
		    strcmp((char *)arpt_node->name, "airport_name") != 0)
			continue;
		load_airport(cdb, arpt_node, city_id, state_id);
	}
	xmlFree(city_id);
}

static void
load_state(chartdb_t *cdb, const xmlNode *state_node)
{
	char *state_id = (char *)xmlGetProp(state_node, (xmlChar *)"ID");

	if (state_id == NULL)
		return;

	for (const xmlNode *city_node = state_node->children; city_node != NULL;
	    city_node = city_node->next) {
		if (city_node->name == NULL ||
		    strcmp((char *)city_node->name, "city_name") != 0)
			continue;
		load_city(cdb, city_node, state_id);
	}
	xmlFree(state_id);
}

static bool_t
load_index(chartdb_t *cdb)
{
	char *index_path = mk_index_path(cdb);
	xmlDoc *doc = NULL;
	xmlXPathContext *xpath_ctx = NULL;
	xmlXPathObject *states_obj = NULL;

	doc = xmlParseFile(index_path);
	if (doc == NULL) {
		logMsg("Error parsing chart index %s: XML parsing error",
		    index_path);
		goto errout;
	}
	xpath_ctx = xmlXPathNewContext(doc);
	if (xpath_ctx == NULL) {
		logMsg("Error creating XPath context for document %s.",
		    index_path);
		goto errout;
	}
	states_obj = xmlXPathEvalExpression((xmlChar *)
	    "/digital_tpp/state_code", xpath_ctx);
	for (int i = 0; i < states_obj->nodesetval->nodeNr; i++)
		load_state(cdb, states_obj->nodesetval->nodeTab[i]);
	xmlXPathFreeObject(states_obj);
	states_obj = NULL;

	xmlXPathFreeContext(xpath_ctx);
	xmlFreeDoc(doc);
	free(index_path);

	return (B_TRUE);
errout:
	if (states_obj != NULL)
		xmlXPathFreeObject(states_obj);
	if (xpath_ctx != NULL)
		xmlXPathFreeContext(xpath_ctx);
	if (doc != NULL)
		xmlFreeDoc(doc);
	free(index_path);

	return (B_FALSE);
}

bool_t
chart_faa_init(chartdb_t *cdb)
{
	/* FAA charts have unique filenames, so switch to flat DB */
	cdb->flat_db = B_TRUE;

	if (!update_index(cdb) || !load_index(cdb))
		goto errout;

	return (B_TRUE);

errout:
	chart_faa_fini(cdb);
	return (B_FALSE);
}

void
chart_faa_fini(chartdb_t *cdb)
{
	UNUSED(cdb);
}

bool_t
chart_faa_get_chart(chart_t *chart)
{
	chartdb_t *cdb;
	chart_arpt_t *arpt;
	char *filepath;
	bool_t result;
	char url[128];

	ASSERT(chart->arpt != NULL);
	arpt = chart->arpt;
	ASSERT(arpt->db != NULL);
	cdb = arpt->db;

	filepath = chartdb_mkpath(chart);
	snprintf(url, sizeof (url), CHART_URL, cdb->airac, chart->filename);
	result = chart_download(cdb, url, filepath, "Error downloading chart",
	    NULL);
	if (!result && file_exists(filepath, NULL)) {
		logMsg("WARNING: failed to contact FAA servers to refresh "
		    "chart \"%s\". However, we appear to still have a locally "
		    "cached copy of this chart available, so I will display "
		    "that one instead.", chart->filename);
		result = B_TRUE;
	}
	free(filepath);

	return (result);
}

static char *
get_metar_taf_common(chartdb_t *cdb, const char *icao, const char *source,
    const char *node_name)
{
	dl_info_t info;
	char url[256];
	char error_reason[128];
	xmlDoc *doc = NULL;
	xmlXPathContext *xpath_ctx = NULL;
	xmlXPathObject *xpath_obj = NULL;
	char query[128];
	char *result;

	snprintf(url, sizeof (url), "https://aviationweather.gov/adds/"
	    "dataserver_current/httpparam?dataSource=%s&"
	    "requestType=retrieve&format=xml&stationString=%s&hoursBeforeNow=1",
	    source, icao);
	snprintf(error_reason, sizeof (error_reason), "Error downloading %s",
	    node_name);
	snprintf(query, sizeof (query), "/response/data/%s/raw_text",
	    node_name);

	if (!chart_download(cdb, url, NULL, error_reason, &info))
		return (NULL);

	doc = xmlParseMemory((char *)info.buf, info.bufsz);
	if (doc == NULL) {
		logMsg("Error parsing %s: XML parsing error", node_name);
		goto errout;
	}
	xpath_ctx = xmlXPathNewContext(doc);
	if (xpath_ctx == NULL) {
		logMsg("Error creating XPath context for XML");
		goto errout;
	}
	xpath_obj = xmlXPathEvalExpression((xmlChar *)query, xpath_ctx);
	if (xpath_obj->nodesetval->nodeNr == 0 ||
	    xpath_obj->nodesetval->nodeTab[0]->children == NULL ||
	    xpath_obj->nodesetval->nodeTab[0]->children->content == NULL) {
		logMsg("Error parsing %s, valid but incorrect XML structure",
		    node_name);
		goto errout;
	}
	result = strdup(
	    (char *)xpath_obj->nodesetval->nodeTab[0]->children->content);
	xmlXPathFreeObject(xpath_obj);
	xmlXPathFreeContext(xpath_ctx);
	xmlFreeDoc(doc);
	free(info.buf);

	return (result);
errout:
	if (xpath_obj != NULL)
		xmlXPathFreeObject(xpath_obj);
	if (xpath_ctx != NULL)
		xmlXPathFreeContext(xpath_ctx);
	if (doc != NULL)
		xmlFreeDoc(doc);
	free(info.buf);

	return (NULL);
}

char *
chart_faa_get_metar(chartdb_t *cdb, const char *icao)
{
	return (get_metar_taf_common(cdb, icao, "metars", "METAR"));
}

char *
chart_faa_get_taf(chartdb_t *cdb, const char *icao)
{
	return (get_metar_taf_common(cdb, icao, "tafs", "TAF"));
}
