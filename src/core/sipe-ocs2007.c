/**
 * @file sipe-ocs2007.c
 *
 * pidgin-sipe
 *
 * Copyright (C) 2011 SIPE Project <http://sipe.sourceforge.net/>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * OCS2007+ specific code
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>

#include "sipe-common.h"
#include "http-conn.h"
#include "sipmsg.h"
#include "sip-csta.h"
#include "sip-transport.h"
#include "sipe-backend.h"
#include "sipe-buddy.h"
#include "sipe-cal.h"
#include "sipe-core.h"
#include "sipe-core-private.h"
#include "sipe-ews.h"
#include "sipe-groupchat.h"
#include "sipe-nls.h"
#include "sipe-ocs2007.h"
#include "sipe-schedule.h"
#include "sipe-status.h"
#include "sipe-utils.h"
#include "sipe-xml.h"

#define _SIPE_NEED_ACTIVITIES /* ugly hack :-( */
#include "sipe.h"

static void send_presence_publish(struct sipe_core_private *sipe_private,
				  const char *publications);

static void free_publication(struct sipe_publication *publication)
{
	g_free(publication->category);
	g_free(publication->cal_event_hash);
	g_free(publication->note);

	g_free(publication->working_hours_xml_str);
	g_free(publication->fb_start_str);
	g_free(publication->free_busy_base64);

	g_free(publication);
}

struct hash_table_delete_payload {
	GHashTable *hash_table;
	guint container;
};

static void sipe_remove_category_container_publications_cb(const gchar *name,
							   struct sipe_publication *publication,
							   struct hash_table_delete_payload *payload)
{
	if (publication->container == payload->container) {
		g_hash_table_remove(payload->hash_table, name);
	}
}

static void sipe_remove_category_container_publications(GHashTable *our_publications,
							const gchar *category,
							guint container)
{
	struct hash_table_delete_payload payload;
	payload.hash_table = g_hash_table_lookup(our_publications, category);

	if (!payload.hash_table) return;

	payload.container = container;
	g_hash_table_foreach(payload.hash_table,
			     (GHFunc)sipe_remove_category_container_publications_cb,
			     &payload);
}

/** MS-PRES container */
struct sipe_container {
	guint id;
	guint version;
	GSList *members;
};

/** MS-PRES container member */
struct sipe_container_member {
	/** user, domain, sameEnterprise, federated, publicCloud; everyone */
	gchar *type;
	gchar *value;
};

static const guint containers[] = {32000, 400, 300, 200, 100};
#define CONTAINERS_LEN (sizeof(containers) / sizeof(guint))

guint sipe_ocs2007_containers(void)
{
	return(CONTAINERS_LEN);
}

static void free_container_member(struct sipe_container_member *member)
{
	if (!member) return;

	g_free(member->type);
	g_free(member->value);
	g_free(member);
}

void sipe_ocs2007_free_container(struct sipe_container *container)
{
	GSList *entry;

	if (!container) return;

	entry = container->members;
	while (entry) {
		void *data = entry->data;
		entry = g_slist_remove(entry, data);
		free_container_member((struct sipe_container_member *)data);
	}
	g_free(container);
}

struct sipe_container *sipe_ocs2007_create_container(guint index,
						     const gchar *member_type,
						     const gchar *member_value,
						     gboolean is_group)
{
	struct sipe_container *container = g_new0(struct sipe_container, 1);
	struct sipe_container_member *member = g_new0(struct sipe_container_member, 1);

	container->id = is_group ? (guint) -1 : containers[index];
	container->members = g_slist_append(container->members, member);
	member->type = g_strdup(member_type);
	member->value = g_strdup(member_value);

	return(container);
}

void sipe_ocs2007_free(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;

	if (sip->containers) {
		GSList *entry = sip->containers;
		while (entry) {
			sipe_ocs2007_free_container((struct sipe_container *)entry->data);
			entry = entry->next;
		}
	}
	g_slist_free(sip->containers);
}

/**
 * Finds locally stored MS-PRES container member
 */
static struct sipe_container_member *
sipe_find_container_member(struct sipe_container *container,
			   const gchar *type,
			   const gchar *value)
{
	struct sipe_container_member *member;
	GSList *entry;

	if (container == NULL || type == NULL) {
		return NULL;
	}

	entry = container->members;
	while (entry) {
		member = entry->data;
		if (sipe_strcase_equal(member->type, type) &&
		    sipe_strcase_equal(member->value, value))
		{
			return member;
		}
		entry = entry->next;
	}
	return NULL;
}

/**
 * Finds locally stored MS-PRES container by id
 */
static struct sipe_container *sipe_find_container(struct sipe_core_private *sipe_private,
						  guint id)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	struct sipe_container *container;
	GSList *entry;

	if (sip == NULL) {
		return NULL;
	}

	entry = sip->containers;
	while (entry) {
		container = entry->data;
		if (id == container->id) {
			return container;
		}
		entry = entry->next;
	}
	return NULL;
}

static int sipe_find_member_access_level(struct sipe_core_private *sipe_private,
					 const gchar *type,
					 const gchar *value)
{
	unsigned int i = 0;
	const gchar *value_mod = value;

	if (!type) return -1;

	if (sipe_strequal("user", type)) {
		value_mod = sipe_get_no_sip_uri(value);
	}

	for (i = 0; i < CONTAINERS_LEN; i++) {
		struct sipe_container_member *member;
		struct sipe_container *container = sipe_find_container(sipe_private, containers[i]);
		if (!container) continue;

		member = sipe_find_container_member(container, type, value_mod);
		if (member) return containers[i];
	}

	return -1;
}

/**
 * Returns pointer to domain part in provided Email URL
 *
 * @param email an email URL. Example: first.last@hq.company.com
 * @return pointer to domain part of email URL. Coresponding example: hq.company.com
 *
 * Doesn't allocate memory
 */
static const gchar *sipe_get_domain(const gchar *email)
{
	gchar *tmp;

	if (!email) return NULL;

	tmp = strstr(email, "@");

	if (tmp && ((tmp+1) < (email + strlen(email)))) {
		return tmp+1;
	} else {
		return NULL;
	}
}

/* @TODO: replace with binary search for faster access? */
/** source: http://support.microsoft.com/kb/897567 */
static const gchar * const public_domains[] = {
	"aol.com", "icq.com", "love.com", "mac.com", "br.live.com",
	"hotmail.co.il", "hotmail.co.jp", "hotmail.co.th", "hotmail.co.uk",
	"hotmail.com", "hotmail.com.ar", "hotmail.com.tr", "hotmail.es",
	"hotmail.de", "hotmail.fr", "hotmail.it", "live.at", "live.be",
	"live.ca", "live.cl", "live.cn", "live.co.in", "live.co.kr",
	"live.co.uk", "live.co.za", "live.com", "live.com.ar", "live.com.au",
	"live.com.co", "live.com.mx", "live.com.my", "live.com.pe",
	"live.com.ph", "live.com.pk", "live.com.pt", "live.com.sg",
	"live.com.ve", "live.de", "live.dk", "live.fr", "live.hk", "live.ie",
	"live.in", "live.it", "live.jp", "live.nl", "live.no", "live.ph",
	"live.ru", "live.se", "livemail.com.br", "livemail.tw",
	"messengeruser.com", "msn.com", "passport.com", "sympatico.ca",
	"tw.live.com", "webtv.net", "windowslive.com", "windowslive.es",
	"yahoo.com",
	NULL};

static gboolean sipe_is_public_domain(const gchar *domain)
{
	int i = 0;
	while (public_domains[i]) {
		if (sipe_strcase_equal(public_domains[i], domain)) {
			return TRUE;
		}
		i++;
	}
	return FALSE;
}

/**
 * Access Levels
 * 32000 - Blocked
 * 400   - Personal
 * 300   - Team
 * 200   - Company
 * 100   - Public
 */
const gchar *sipe_ocs2007_access_level_name(guint id)
{
	switch (id) {
		case 32000: return _("Blocked");
		case 400:   return _("Personal");
		case 300:   return _("Team");
		case 200:   return _("Company");
		case 100:   return _("Public");
	}
	return _("Unknown");
}

int sipe_ocs2007_container_id(guint index)
{
	return(containers[index]);
}

/** Member type: user, domain, sameEnterprise, federated, publicCloud; everyone */
int sipe_ocs2007_find_access_level(struct sipe_core_private *sipe_private,
				   const gchar *type,
				   const gchar *value,
				   gboolean *is_group_access)
{
	int container_id = -1;

	if (sipe_strequal("user", type)) {
		const char *domain;
		const char *no_sip_uri = sipe_get_no_sip_uri(value);

		container_id = sipe_find_member_access_level(sipe_private, "user", no_sip_uri);
		if (container_id >= 0) {
			if (is_group_access) *is_group_access = FALSE;
			return container_id;
		}

		domain = sipe_get_domain(no_sip_uri);
		container_id = sipe_find_member_access_level(sipe_private, "domain", domain);
		if (container_id >= 0)  {
			if (is_group_access) *is_group_access = TRUE;
			return container_id;
		}

		container_id = sipe_find_member_access_level(sipe_private, "sameEnterprise", NULL);
		if ((container_id >= 0) && sipe_strcase_equal(sipe_private->public.sip_domain, domain)) {
			if (is_group_access) *is_group_access = TRUE;
			return container_id;
		}

		container_id = sipe_find_member_access_level(sipe_private, "publicCloud", NULL);
		if ((container_id >= 0) && sipe_is_public_domain(domain)) {
			if (is_group_access) *is_group_access = TRUE;
			return container_id;
		}

		container_id = sipe_find_member_access_level(sipe_private, "everyone", NULL);
		if ((container_id >= 0)) {
			if (is_group_access) *is_group_access = TRUE;
			return container_id;
		}
	} else {
		container_id = sipe_find_member_access_level(sipe_private, type, value);
		if (is_group_access) *is_group_access = FALSE;
	}

	return container_id;
}

GSList *sipe_ocs2007_get_access_domains(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	struct sipe_container *container;
	struct sipe_container_member *member;
	GSList *entry;
	GSList *entry2;
	GSList *res = NULL;

	if (!sip) return NULL;

	entry = sip->containers;
	while (entry) {
		container = entry->data;

		entry2 = container->members;
		while (entry2) {
			member = entry2->data;
			if (sipe_strcase_equal(member->type, "domain"))
			{
				res = slist_insert_unique_sorted(res, g_strdup(member->value), (GCompareFunc)g_ascii_strcasecmp);
			}
			entry2 = entry2->next;
		}
		entry = entry->next;
	}
	return res;
}

static void sipe_send_container_members_prepare(const guint container_id,
						const guint container_version,
						const gchar *action,
						const gchar *type,
						const gchar *value,
						char **container_xmls)
{
	gchar *value_str = value ? g_strdup_printf(" value=\"%s\"", value) : g_strdup("");
	gchar *body;

	if (!container_xmls) return;

	body = g_strdup_printf(
		"<container id=\"%d\" version=\"%d\"><member action=\"%s\" type=\"%s\"%s/></container>",
		container_id,
		container_version,
		action,
		type,
		value_str);
	g_free(value_str);

	if ((*container_xmls) == NULL) {
		*container_xmls = body;
	} else {
		char *tmp = *container_xmls;

		*container_xmls = g_strconcat(*container_xmls, body, NULL);
		g_free(tmp);
		g_free(body);
	}
}

static void sipe_send_set_container_members(struct sipe_core_private *sipe_private,
					    char *container_xmls)
{
	gchar *self;
	gchar *contact;
	gchar *hdr;
	gchar *body;

	if (!container_xmls) return;

	self = sip_uri_self(sipe_private);
	body = g_strdup_printf(
		"<setContainerMembers xmlns=\"http://schemas.microsoft.com/2006/09/sip/container-management\">"
		"%s"
		"</setContainerMembers>",
		container_xmls);

	contact = get_contact(sipe_private);
	hdr = g_strdup_printf("Contact: %s\r\n"
			      "Content-Type: application/msrtc-setcontainermembers+xml\r\n", contact);
	g_free(contact);

	sip_transport_service(sipe_private,
			      self,
			      hdr,
			      body,
			      NULL);

	g_free(hdr);
	g_free(body);
	g_free(self);
}

/**
  * @param container_id	a new access level. If -1 then current access level
  * 			is just removed (I.e. the member is removed from all containers).
  * @param type		a type of member. E.g. "user", "sameEnterprise", etc.
  * @param value	a value for member. E.g. SIP URI for "user" member type.
  */
void sipe_ocs2007_change_access_level(struct sipe_core_private *sipe_private,
				      const int container_id,
				      const gchar *type,
				      const gchar *value)
{
	unsigned int i;
	int current_container_id = -1;
	char *container_xmls = NULL;

	/* for each container: find/delete */
	for (i = 0; i < CONTAINERS_LEN; i++) {
		struct sipe_container_member *member;
		struct sipe_container *container = sipe_find_container(sipe_private, containers[i]);

		if (!container) continue;

		member = sipe_find_container_member(container, type, value);
		if (member) {
			current_container_id = containers[i];
			/* delete/publish current access level */
			if (container_id < 0 || container_id != current_container_id) {
				sipe_send_container_members_prepare(current_container_id, container->version, "remove", type, value, &container_xmls);
				/* remove member from our cache, to be able to recalculate AL below */
				container->members = g_slist_remove(container->members, member);
				current_container_id = -1;
			}
		}
	}

	/* recalculate AL below */
	current_container_id = sipe_ocs2007_find_access_level(sipe_private, type, value, NULL);

	/* assign/publish new access level */
	if (container_id != current_container_id && container_id >= 0) {
		struct sipe_container *container = sipe_find_container(sipe_private, container_id);
		guint version = container ? container->version : 0;

		sipe_send_container_members_prepare(container_id, version, "add", type, value, &container_xmls);
	}

	if (container_xmls) {
		sipe_send_set_container_members(sipe_private, container_xmls);
	}
	g_free(container_xmls);
}

void sipe_ocs2007_change_access_level_from_container(struct sipe_core_private *sipe_private,
						     struct sipe_container *container)
{
	struct sipe_container_member *member;

	if (!container || !container->members) return;

	member = ((struct sipe_container_member *)container->members->data);

	if (!member->type) return;

	SIPE_DEBUG_INFO("sipe_ocs2007_change_access_level_from_container: container->id=%d, member->type=%s, member->value=%s",
			container->id, member->type, member->value ? member->value : "");

	sipe_ocs2007_change_access_level(sipe_private,
					 container->id,
					 member->type,
					 member->value);

}

void sipe_ocs2007_change_access_level_for_domain(struct sipe_core_private *sipe_private,
						 const gchar *domain,
						 guint index)
{
	/* move Blocked first */
	guint i            = (index == 4) ? 0 : index + 1;
	guint container_id = containers[i];

	SIPE_DEBUG_INFO("sipe_ocs2007_change_access_level_from_id: domain=%s, container_id=(%d)%d",
			domain ? domain : "", index, container_id);

	sipe_ocs2007_change_access_level(sipe_private,
					 container_id,
					 "domain",
					 domain);

}

/**
 * Schedules process of self status publish
 * based on own calendar information.
 * Should be scheduled to the beginning of every
 * 15 min interval, like:
 * 13:00, 13:15, 13:30, 13:45, etc.
 *
 */
static void schedule_publish_update(struct sipe_core_private *sipe_private,
				    time_t calculate_from)
{
	int interval = 5*60;
	/** start of the beginning of closest 5 min interval. */
	time_t next_start = ((time_t)((int)((int)calculate_from)/interval + 1)*interval);

	SIPE_DEBUG_INFO("sipe_sched_calendar_status_self_publish: calculate_from time: %s",
			asctime(localtime(&calculate_from)));
	SIPE_DEBUG_INFO("sipe_sched_calendar_status_self_publish: next start time    : %s",
			asctime(localtime(&next_start)));

	sipe_schedule_seconds(sipe_private,
			      "<+2007-cal-status>",
			      NULL,
			      next_start - time(NULL),
			      sipe_ocs2007_presence_publish,
			      NULL);
}

/**
 * An availability XML entry for SIPE_PUB_XML_STATE_CALENDAR
 * @param availability		(%d) Ex.: 6500
 */
#define SIPE_PUB_XML_STATE_CALENDAR_AVAIL \
"<availability>%d</availability>"
/**
 * An activity XML entry for SIPE_PUB_XML_STATE_CALENDAR
 * @param token			(%s) Ex.: in-a-meeting
 * @param minAvailability_attr	(%s) Ex.: minAvailability="6500"
 * @param maxAvailability_attr	(%s) Ex.: maxAvailability="8999" or none
 */
#define SIPE_PUB_XML_STATE_CALENDAR_ACTIVITY \
"<activity token=\"%s\" %s %s></activity>"
/**
 * Publishes 'calendarState' category.
 * @param instance		(%u) Ex.: 1339299275
 * @param version		(%u) Ex.: 1
 * @param uri			(%s) Ex.: john@contoso.com
 * @param start_time_str	(%s) Ex.: 2008-01-11T19:00:00Z
 * @param availability		(%s) XML string as SIPE_PUB_XML_STATE_CALENDAR_AVAIL
 * @param activity		(%s) XML string as SIPE_PUB_XML_STATE_CALENDAR_ACTIVITY
 * @param meeting_subject	(%s) Ex.: Customer Meeting
 * @param meeting_location	(%s) Ex.: Conf Room 100
 *
 * @param instance		(%u) Ex.: 1339299275
 * @param version		(%u) Ex.: 1
 * @param uri			(%s) Ex.: john@contoso.com
 * @param start_time_str	(%s) Ex.: 2008-01-11T19:00:00Z
 * @param availability		(%s) XML string as SIPE_PUB_XML_STATE_CALENDAR_AVAIL
 * @param activity		(%s) XML string as SIPE_PUB_XML_STATE_CALENDAR_ACTIVITY
 * @param meeting_subject	(%s) Ex.: Customer Meeting
 * @param meeting_location	(%s) Ex.: Conf Room 100
 */
#define SIPE_PUB_XML_STATE_CALENDAR \
	"<publication categoryName=\"state\" instance=\"%u\" container=\"2\" version=\"%u\" expireType=\"endpoint\">"\
		"<state xmlns=\"http://schemas.microsoft.com/2006/09/sip/state\" manual=\"false\" uri=\"%s\" startTime=\"%s\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"calendarState\">"\
			"%s"\
			"%s"\
			"<endpointLocation/>"\
			"<meetingSubject>%s</meetingSubject>"\
			"<meetingLocation>%s</meetingLocation>"\
		"</state>"\
	"</publication>"\
	"<publication categoryName=\"state\" instance=\"%u\" container=\"3\" version=\"%u\" expireType=\"endpoint\">"\
		"<state xmlns=\"http://schemas.microsoft.com/2006/09/sip/state\" manual=\"false\" uri=\"%s\" startTime=\"%s\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"calendarState\">"\
			"%s"\
			"%s"\
			"<endpointLocation/>"\
			"<meetingSubject>%s</meetingSubject>"\
			"<meetingLocation>%s</meetingLocation>"\
		"</state>"\
	"</publication>"
/**
 * Publishes to clear 'calendarState' category
 * @param instance		(%u) Ex.: 1251210982
 * @param version		(%u) Ex.: 1
 */
#define SIPE_PUB_XML_STATE_CALENDAR_CLEAR \
	"<publication categoryName=\"state\" instance=\"%u\" container=\"2\" version=\"%u\" expireType=\"endpoint\" expires=\"0\"/>"\
	"<publication categoryName=\"state\" instance=\"%u\" container=\"3\" version=\"%u\" expireType=\"endpoint\" expires=\"0\"/>"

/**
 * Publishes to clear any category
 * @param category_name		(%s) Ex.: state
 * @param instance		(%u) Ex.: 536870912
 * @param container		(%u) Ex.: 3
 * @param version		(%u) Ex.: 1
 * @param expireType		(%s) Ex.: static
 */
#define SIPE_PUB_XML_PUBLICATION_CLEAR \
	"<publication categoryName=\"%s\" instance=\"%u\" container=\"%u\" version=\"%u\" expireType=\"%s\" expires=\"0\"/>"

/**
 * Publishes 'note' category.
 * @param instance		(%u) Ex.: 2135971629; 0 for personal
 * @param container		(%u) Ex.: 200
 * @param version		(%u) Ex.: 2
 * @param type			(%s) Ex.: personal or OOF
 * @param startTime_attr	(%s) Ex.: startTime="2008-01-11T19:00:00Z"
 * @param endTime_attr		(%s) Ex.: endTime="2008-01-15T19:00:00Z"
 * @param body			(%s) Ex.: In the office
 */
#define SIPE_PUB_XML_NOTE \
	"<publication categoryName=\"note\" instance=\"%u\" container=\"%u\" version=\"%d\" expireType=\"static\">"\
		"<note xmlns=\"http://schemas.microsoft.com/2006/09/sip/note\">"\
			"<body type=\"%s\" uri=\"\"%s%s>%s</body>"\
		"</note>"\
	"</publication>"

/**
 * Only Busy and OOF calendar event are published.
 * Different instances are used for that.
 *
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_state_calendar(struct sipe_core_private *sipe_private,
						       struct sipe_cal_event *event,
						       const char *uri,
						       int cal_satus)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	gchar *start_time_str;
	int availability = 0;
	gchar *res;
	gchar *tmp = NULL;
	guint instance = (cal_satus == SIPE_CAL_OOF) ?
		sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_CALENDAR_OOF) :
		sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_CALENDAR);

	/* key is <category><instance><container> */
	gchar *key_2 = g_strdup_printf("<%s><%u><%u>", "state", instance, 2);
	gchar *key_3 = g_strdup_printf("<%s><%u><%u>", "state", instance, 3);
	struct sipe_publication *publication_2 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "state"), key_2);
	struct sipe_publication *publication_3 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "state"), key_3);

	g_free(key_2);
	g_free(key_3);

	if (!publication_3 && !event) { /* was nothing, have nothing, exiting */
		SIPE_DEBUG_INFO("sipe_publish_get_category_state_calendar: "
				"Exiting as no publication and no event for cal_satus:%d", cal_satus);
		return NULL;
	}

	if (event &&
	    publication_3 &&
	    (publication_3->availability == availability) &&
	    sipe_strequal(publication_3->cal_event_hash, (tmp = sipe_cal_event_hash(event))))
	{
		g_free(tmp);
		SIPE_DEBUG_INFO("sipe_publish_get_category_state_calendar: "
				"cal state has NOT changed for cal_satus:%d. Exiting.", cal_satus);
		return NULL; /* nothing to update */
	}
	g_free(tmp);

	if (event &&
	    (event->cal_status == SIPE_CAL_BUSY ||
	     event->cal_status == SIPE_CAL_OOF))
	{
		gchar *availability_xml_str = NULL;
		gchar *activity_xml_str = NULL;
		gchar *escaped_subject  = event->subject  ? g_markup_escape_text(event->subject,  -1) : NULL;
		gchar *escaped_location = event->location ? g_markup_escape_text(event->location, -1) : NULL;

		if (event->cal_status == SIPE_CAL_BUSY) {
			availability_xml_str = g_strdup_printf(SIPE_PUB_XML_STATE_CALENDAR_AVAIL, 6500);
		}

		if (event->cal_status == SIPE_CAL_BUSY && event->is_meeting) {
			activity_xml_str = g_strdup_printf(SIPE_PUB_XML_STATE_CALENDAR_ACTIVITY,
							   sipe_activity_to_token(SIPE_ACTIVITY_IN_MEETING),
							   "minAvailability=\"6500\"",
							   "maxAvailability=\"8999\"");
		} else if (event->cal_status == SIPE_CAL_OOF) {
			activity_xml_str = g_strdup_printf(SIPE_PUB_XML_STATE_CALENDAR_ACTIVITY,
							   sipe_activity_to_token(SIPE_ACTIVITY_OOF),
							   "minAvailability=\"12000\"",
							   "");
		}
		start_time_str = sipe_utils_time_to_str(event->start_time);

		res = g_strdup_printf(SIPE_PUB_XML_STATE_CALENDAR,
					instance,
					publication_2 ? publication_2->version : 0,
					uri,
					start_time_str,
					availability_xml_str ? availability_xml_str : "",
					activity_xml_str ? activity_xml_str : "",
					escaped_subject  ? escaped_subject  : "",
					escaped_location ? escaped_location : "",

					instance,
					publication_3 ? publication_3->version : 0,
					uri,
					start_time_str,
					availability_xml_str ? availability_xml_str : "",
					activity_xml_str ? activity_xml_str : "",
					escaped_subject  ? escaped_subject  : "",
					escaped_location ? escaped_location : ""
					);
		g_free(escaped_location);
		g_free(escaped_subject);
		g_free(start_time_str);
		g_free(availability_xml_str);
		g_free(activity_xml_str);

	}
	else /* including !event, SIPE_CAL_FREE, SIPE_CAL_TENTATIVE */
	{
		res = g_strdup_printf(SIPE_PUB_XML_STATE_CALENDAR_CLEAR,
					instance,
					publication_2 ? publication_2->version : 0,

					instance,
					publication_3 ? publication_3->version : 0
					);
	}

	return res;
}

/**
 * Returns 'note' XML part for publication.
 * Must be g_free'd after use.
 *
 * Protocol format for Note is plain text.
 *
 * @param note a note in Sipe internal HTML format
 * @param note_type either personal or OOF
 */
static gchar *sipe_publish_get_category_note(struct sipe_core_private *sipe_private,
					     const char *note, /* html */
					     const char *note_type,
					     time_t note_start,
					     time_t note_end)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	guint instance = sipe_strequal("OOF", note_type) ? sipe_get_pub_instance(sipe_private, SIPE_PUB_NOTE_OOF) : 0;
	/* key is <category><instance><container> */
	gchar *key_note_200 = g_strdup_printf("<%s><%u><%u>", "note", instance, 200);
	gchar *key_note_300 = g_strdup_printf("<%s><%u><%u>", "note", instance, 300);
	gchar *key_note_400 = g_strdup_printf("<%s><%u><%u>", "note", instance, 400);

	struct sipe_publication *publication_note_200 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "note"), key_note_200);
	struct sipe_publication *publication_note_300 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "note"), key_note_300);
	struct sipe_publication *publication_note_400 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "note"), key_note_400);

	char *tmp = note ? sipe_backend_markup_strip_html(note) : NULL;
	char *n1 = tmp ? g_markup_escape_text(tmp, -1) : NULL;
	const char *n2 = publication_note_200 ? publication_note_200->note : NULL;
	char *res, *tmp1, *tmp2, *tmp3;
	char *start_time_attr;
	char *end_time_attr;

	g_free(tmp);
	tmp = NULL;
	g_free(key_note_200);
	g_free(key_note_300);
	g_free(key_note_400);

	/* we even need to republish empty note */
	if (sipe_strequal(n1, n2))
	{
		SIPE_DEBUG_INFO_NOFORMAT("sipe_publish_get_category_note: note has NOT changed. Exiting.");
		g_free(n1);
		return NULL; /* nothing to update */
	}

	start_time_attr = note_start ? g_strdup_printf(" startTime=\"%s\"", (tmp = sipe_utils_time_to_str(note_start))) : NULL;
	g_free(tmp);
	tmp = NULL;
	end_time_attr = note_end ? g_strdup_printf(" endTime=\"%s\"", (tmp = sipe_utils_time_to_str(note_end))) : NULL;
	g_free(tmp);

	if (n1) {
		tmp1 = g_strdup_printf(SIPE_PUB_XML_NOTE,
				       instance,
				       200,
				       publication_note_200 ? publication_note_200->version : 0,
				       note_type,
				       start_time_attr ? start_time_attr : "",
				       end_time_attr ? end_time_attr : "",
				       n1);

		tmp2 = g_strdup_printf(SIPE_PUB_XML_NOTE,
				       instance,
				       300,
				       publication_note_300 ? publication_note_300->version : 0,
				       note_type,
				       start_time_attr ? start_time_attr : "",
				       end_time_attr ? end_time_attr : "",
				       n1);

		tmp3 = g_strdup_printf(SIPE_PUB_XML_NOTE,
				       instance,
				       400,
				       publication_note_400 ? publication_note_400->version : 0,
				       note_type,
				       start_time_attr ? start_time_attr : "",
				       end_time_attr ? end_time_attr : "",
				       n1);
	} else {
		tmp1 = g_strdup_printf( SIPE_PUB_XML_PUBLICATION_CLEAR,
					"note",
					instance,
					200,
					publication_note_200 ? publication_note_200->version : 0,
					"static");
		tmp2 = g_strdup_printf( SIPE_PUB_XML_PUBLICATION_CLEAR,
					"note",
					instance,
					300,
					publication_note_200 ? publication_note_200->version : 0,
					"static");
		tmp3 = g_strdup_printf( SIPE_PUB_XML_PUBLICATION_CLEAR,
					"note",
					instance,
					400,
					publication_note_200 ? publication_note_200->version : 0,
					"static");
	}
	res =  g_strconcat(tmp1, tmp2, tmp3, NULL);

	g_free(start_time_attr);
	g_free(end_time_attr);
	g_free(tmp1);
	g_free(tmp2);
	g_free(tmp3);
	g_free(n1);

	return res;
}

/**
 * Publishes 'calendarData' category's WorkingHours.
 *
 * @param version	        (%u)  Ex.: 1
 * @param email	                (%s)  Ex.: alice@cosmo.local
 * @param working_hours_xml_str	(%s)  Ex.: <WorkingHours xmlns=.....
 *
 * @param version	        (%u)
 *
 * @param version	        (%u)
 * @param email	                (%s)
 * @param working_hours_xml_str	(%s)
 *
 * @param version	        (%u)
 * @param email	                (%s)
 * @param working_hours_xml_str	(%s)
 *
 * @param version	        (%u)
 * @param email	                (%s)
 * @param working_hours_xml_str	(%s)
 *
 * @param version	        (%u)
 */
#define SIPE_PUB_XML_WORKING_HOURS \
	"<publication categoryName=\"calendarData\" instance=\"0\" container=\"1\" version=\"%d\" expireType=\"static\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">%s"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"0\" container=\"100\" version=\"%d\" expireType=\"static\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\"/>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"0\" container=\"200\" version=\"%d\" expireType=\"static\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">%s"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"0\" container=\"300\" version=\"%d\" expireType=\"static\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">%s"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"0\" container=\"400\" version=\"%d\" expireType=\"static\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">%s"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"0\" container=\"32000\" version=\"%d\" expireType=\"static\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\"/>"\
	"</publication>"

/**
 * Returns 'calendarData' XML part with WorkingHours for publication.
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_cal_working_hours(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	struct sipe_calendar* cal = sip->cal;

	/* key is <category><instance><container> */
	gchar *key_cal_1     = g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 1);
	gchar *key_cal_100   = g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 100);
	gchar *key_cal_200   = g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 200);
	gchar *key_cal_300   = g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 300);
	gchar *key_cal_400   = g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 400);
	gchar *key_cal_32000 = g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 32000);

	struct sipe_publication *publication_cal_1 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_1);
	struct sipe_publication *publication_cal_100 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_100);
	struct sipe_publication *publication_cal_200 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_200);
	struct sipe_publication *publication_cal_300 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_300);
	struct sipe_publication *publication_cal_400 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_400);
	struct sipe_publication *publication_cal_32000 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_32000);

	const char *n1 = cal ? cal->working_hours_xml_str : NULL;
	const char *n2 = publication_cal_300 ? publication_cal_300->working_hours_xml_str : NULL;

	g_free(key_cal_1);
	g_free(key_cal_100);
	g_free(key_cal_200);
	g_free(key_cal_300);
	g_free(key_cal_400);
	g_free(key_cal_32000);

	if (!cal || is_empty(cal->email) || is_empty(cal->working_hours_xml_str)) {
		SIPE_DEBUG_INFO_NOFORMAT("sipe_publish_get_category_cal_working_hours: no data to publish, exiting");
		return NULL;
	}

	if (sipe_strequal(n1, n2))
	{
		SIPE_DEBUG_INFO_NOFORMAT("sipe_publish_get_category_cal_working_hours: WorkingHours has NOT changed. Exiting.");
		return NULL; /* nothing to update */
	}

	return g_strdup_printf(SIPE_PUB_XML_WORKING_HOURS,
				/* 1 */
				publication_cal_1 ? publication_cal_1->version : 0,
				cal->email,
				cal->working_hours_xml_str,
				/* 100 - Public */
				publication_cal_100 ? publication_cal_100->version : 0,
				/* 200 - Company */
				publication_cal_200 ? publication_cal_200->version : 0,
				cal->email,
				cal->working_hours_xml_str,
				/* 300 - Team */
				publication_cal_300 ? publication_cal_300->version : 0,
				cal->email,
				cal->working_hours_xml_str,
				/* 400 - Personal */
				publication_cal_400 ? publication_cal_400->version : 0,
				cal->email,
				cal->working_hours_xml_str,
				/* 32000 - Blocked */
				publication_cal_32000 ? publication_cal_32000->version : 0
			      );
}

/**
 * Publishes 'calendarData' category's FreeBusy.
 *
 * @param instance	        (%u)  Ex.: 1300372959
 * @param version	        (%u)  Ex.: 1
 *
 * @param instance	        (%u)  Ex.: 1300372959
 * @param version	        (%u)  Ex.: 1
 *
 * @param instance	        (%u)  Ex.: 1300372959
 * @param version	        (%u)  Ex.: 1
 * @param email	                (%s)  Ex.: alice@cosmo.local
 * @param fb_start_time_str	(%s)  Ex.: 2009-12-03T00:00:00Z
 * @param free_busy_base64	(%s)  Ex.: AAAAAAAAAAAAAAAAAAAAA.....
 *
 * @param instance	        (%u)  Ex.: 1300372959
 * @param version	        (%u)  Ex.: 1
 * @param email	                (%s)  Ex.: alice@cosmo.local
 * @param fb_start_time_str	(%s)  Ex.: 2009-12-03T00:00:00Z
 * @param free_busy_base64	(%s)  Ex.: AAAAAAAAAAAAAAAAAAAAA.....
 *
 * @param instance	        (%u)  Ex.: 1300372959
 * @param version	        (%u)  Ex.: 1
 * @param email	                (%s)  Ex.: alice@cosmo.local
 * @param fb_start_time_str	(%s)  Ex.: 2009-12-03T00:00:00Z
 * @param free_busy_base64	(%s)  Ex.: AAAAAAAAAAAAAAAAAAAAA.....
 *
 * @param instance	        (%u)  Ex.: 1300372959
 * @param version	        (%u)  Ex.: 1
 */
#define SIPE_PUB_XML_FREE_BUSY \
	"<publication categoryName=\"calendarData\" instance=\"%u\" container=\"1\" version=\"%d\" expireType=\"endpoint\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\"/>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"%u\" container=\"100\" version=\"%d\" expireType=\"endpoint\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\"/>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"%u\" container=\"200\" version=\"%d\" expireType=\"endpoint\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">"\
			"<freeBusy startTime=\"%s\" granularity=\"PT15M\" encodingVersion=\"1\">%s</freeBusy>"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"%u\" container=\"300\" version=\"%d\" expireType=\"endpoint\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">"\
			"<freeBusy startTime=\"%s\" granularity=\"PT15M\" encodingVersion=\"1\">%s</freeBusy>"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"%u\" container=\"400\" version=\"%d\" expireType=\"endpoint\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\" mailboxID=\"%s\">"\
			"<freeBusy startTime=\"%s\" granularity=\"PT15M\" encodingVersion=\"1\">%s</freeBusy>"\
		"</calendarData>"\
	"</publication>"\
	"<publication categoryName=\"calendarData\" instance=\"%u\" container=\"32000\" version=\"%d\" expireType=\"endpoint\">"\
		"<calendarData xmlns=\"http://schemas.microsoft.com/2006/09/sip/calendarData\"/>"\
	"</publication>"

/**
 * Returns 'calendarData' XML part with FreeBusy for publication.
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_cal_free_busy(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	struct sipe_calendar* cal = sip->cal;
	guint cal_data_instance = sipe_get_pub_instance(sipe_private, SIPE_PUB_CALENDAR_DATA);
	char *fb_start_str;
	char *free_busy_base64;
	/* const char *st; */
	/* const char *fb; */
	char *res;

	/* key is <category><instance><container> */
	gchar *key_cal_1     = g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 1);
	gchar *key_cal_100   = g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 100);
	gchar *key_cal_200   = g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 200);
	gchar *key_cal_300   = g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 300);
	gchar *key_cal_400   = g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 400);
	gchar *key_cal_32000 = g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 32000);

	struct sipe_publication *publication_cal_1 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_1);
	struct sipe_publication *publication_cal_100 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_100);
	struct sipe_publication *publication_cal_200 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_200);
	struct sipe_publication *publication_cal_300 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_300);
	struct sipe_publication *publication_cal_400 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_400);
	struct sipe_publication *publication_cal_32000 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "calendarData"), key_cal_32000);

	g_free(key_cal_1);
	g_free(key_cal_100);
	g_free(key_cal_200);
	g_free(key_cal_300);
	g_free(key_cal_400);
	g_free(key_cal_32000);

	if (!cal || is_empty(cal->email) || !cal->fb_start || is_empty(cal->free_busy)) {
		SIPE_DEBUG_INFO_NOFORMAT("sipe_publish_get_category_cal_free_busy: no data to publish, exiting");
		return NULL;
	}

	fb_start_str = sipe_utils_time_to_str(cal->fb_start);
	free_busy_base64 = sipe_cal_get_freebusy_base64(cal->free_busy);

	/* we will rebuplish the same data to refresh publication time,
	 * so if data from multiple sources, most recent will be choosen
	 */
	// st = publication_cal_300 ? publication_cal_300->fb_start_str : NULL;
	// fb = publication_cal_300 ? publication_cal_300->free_busy_base64 : NULL;
	//
	//if (sipe_strequal(st, fb_start_str) && sipe_strequal(fb, free_busy_base64))
	//{
	//	SIPE_DEBUG_INFO_NOFORMAT("sipe_publish_get_category_cal_free_busy: FreeBusy has NOT changed. Exiting.");
	//	g_free(fb_start_str);
	//	g_free(free_busy_base64);
	//	return NULL; /* nothing to update */
	//}

	res = g_strdup_printf(SIPE_PUB_XML_FREE_BUSY,
				/* 1 */
				cal_data_instance,
				publication_cal_1 ? publication_cal_1->version : 0,
				/* 100 - Public */
				cal_data_instance,
				publication_cal_100 ? publication_cal_100->version : 0,
				/* 200 - Company */
				cal_data_instance,
				publication_cal_200 ? publication_cal_200->version : 0,
				cal->email,
				fb_start_str,
				free_busy_base64,
				/* 300 - Team */
				cal_data_instance,
				publication_cal_300 ? publication_cal_300->version : 0,
				cal->email,
				fb_start_str,
				free_busy_base64,
				/* 400 - Personal */
				cal_data_instance,
				publication_cal_400 ? publication_cal_400->version : 0,
				cal->email,
				fb_start_str,
				free_busy_base64,
				/* 32000 - Blocked */
				cal_data_instance,
				publication_cal_32000 ? publication_cal_32000->version : 0
			     );

	g_free(fb_start_str);
	g_free(free_busy_base64);
	return res;
}


/**
 * Publishes 'device' category.
 * @param instance	(%u) Ex.: 1938468728
 * @param version	(%u) Ex.: 1
 * @param endpointId	(%s) Ex.: C707E38E-1E10-5413-94D9-ECAC260A0269
 * @param uri		(%s) Self URI. Ex.: sip:alice7@boston.local
 * @param timezone	(%s) Ex.: 00:00:00+01:00
 * @param machineName	(%s) Ex.: BOSTON-OCS07
 */
#define SIPE_PUB_XML_DEVICE \
	"<publication categoryName=\"device\" instance=\"%u\" container=\"2\" version=\"%u\" expireType=\"endpoint\">"\
		"<device xmlns=\"http://schemas.microsoft.com/2006/09/sip/device\" endpointId=\"%s\">"\
			"<capabilities preferred=\"false\" uri=\"%s\">"\
				"<text capture=\"true\" render=\"true\" publish=\"false\"/>"\
				"<gifInk capture=\"false\" render=\"true\" publish=\"false\"/>"\
				"<isfInk capture=\"false\" render=\"true\" publish=\"false\"/>"\
			"</capabilities>"\
			"<timezone>%s</timezone>"\
			"<machineName>%s</machineName>"\
		"</device>"\
	"</publication>"

/**
 * Returns 'device' XML part for publication.
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_device(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	gchar *uri;
	gchar *doc;
	gchar *uuid = get_uuid(sipe_private);
	guint device_instance = sipe_get_pub_instance(sipe_private, SIPE_PUB_DEVICE);
	/* key is <category><instance><container> */
	gchar *key = g_strdup_printf("<%s><%u><%u>", "device", device_instance, 2);
	struct sipe_publication *publication =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "device"), key);

	g_free(key);

	uri = sip_uri_self(sipe_private);
	doc = g_strdup_printf(SIPE_PUB_XML_DEVICE,
		device_instance,
		publication ? publication->version : 0,
		uuid,
		uri,
		"00:00:00+01:00", /* @TODO make timezone real*/
		g_get_host_name()
	);

	g_free(uri);
	g_free(uuid);

	return doc;
}

/**
 * Publishes 'machineState' category.
 * @param instance	(%u) Ex.: 926460663
 * @param version	(%u) Ex.: 22
 * @param availability	(%d) Ex.: 3500
 * @param instance	(%u) Ex.: 926460663
 * @param version	(%u) Ex.: 22
 * @param availability	(%d) Ex.: 3500
 */
#define SIPE_PUB_XML_STATE_MACHINE \
	"<publication categoryName=\"state\" instance=\"%u\" container=\"2\" version=\"%u\" expireType=\"endpoint\">"\
		"<state xmlns=\"http://schemas.microsoft.com/2006/09/sip/state\" manual=\"false\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"machineState\">"\
			"<availability>%d</availability>"\
			"<endpointLocation/>"\
		"</state>"\
	"</publication>"\
	"<publication categoryName=\"state\" instance=\"%u\" container=\"3\" version=\"%u\" expireType=\"endpoint\">"\
		"<state xmlns=\"http://schemas.microsoft.com/2006/09/sip/state\" manual=\"false\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"machineState\">"\
			"<availability>%d</availability>"\
			"<endpointLocation/>"\
		"</state>"\
	"</publication>"

/**
 * Publishes 'userState' category.
 * @param instance	(%u) User. Ex.: 536870912
 * @param version	(%u) User Container 2. Ex.: 22
 * @param availability	(%d) User Container 2. Ex.: 15500
 * @param instance	(%u) User. Ex.: 536870912
 * @param version	(%u) User Container 3.Ex.: 22
 * @param availability	(%d) User Container 3. Ex.: 15500
 */
#define SIPE_PUB_XML_STATE_USER \
	"<publication categoryName=\"state\" instance=\"%u\" container=\"2\" version=\"%u\" expireType=\"static\">"\
		"<state xmlns=\"http://schemas.microsoft.com/2006/09/sip/state\" manual=\"true\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"userState\">"\
			"<availability>%d</availability>"\
			"<endpointLocation/>"\
		"</state>"\
	"</publication>"\
	"<publication categoryName=\"state\" instance=\"%u\" container=\"3\" version=\"%u\" expireType=\"static\">"\
		"<state xmlns=\"http://schemas.microsoft.com/2006/09/sip/state\" manual=\"true\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xsi:type=\"userState\">"\
			"<availability>%d</availability>"\
			"<endpointLocation/>"\
		"</state>"\
	"</publication>"

/**
 * A service method - use
 * - send_publish_get_category_state_machine and
 * - send_publish_get_category_state_user instead.
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_state(struct sipe_core_private *sipe_private,
					      gboolean is_user_state)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	int availability = sipe_get_availability_by_status(sip->status, NULL);
	guint instance = is_user_state ? sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_USER) :
					 sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_MACHINE);
	/* key is <category><instance><container> */
	gchar *key_2 = g_strdup_printf("<%s><%u><%u>", "state", instance, 2);
	gchar *key_3 = g_strdup_printf("<%s><%u><%u>", "state", instance, 3);
	struct sipe_publication *publication_2 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "state"), key_2);
	struct sipe_publication *publication_3 =
		g_hash_table_lookup(g_hash_table_lookup(sip->our_publications, "state"), key_3);

	g_free(key_2);
	g_free(key_3);

	if (publication_2 && (publication_2->availability == availability))
	{
		SIPE_DEBUG_INFO_NOFORMAT("sipe_publish_get_category_state: state has NOT changed. Exiting.");
		return NULL; /* nothing to update */
	}

	return g_strdup_printf( is_user_state ? SIPE_PUB_XML_STATE_USER : SIPE_PUB_XML_STATE_MACHINE,
				instance,
				publication_2 ? publication_2->version : 0,
				availability,
				instance,
				publication_3 ? publication_3->version : 0,
				availability);
}

/**
 * Returns 'machineState' XML part for publication.
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_state_machine(struct sipe_core_private *sipe_private)
{
	return sipe_publish_get_category_state(sipe_private, FALSE);
}

/**
 * Returns 'userState' XML part for publication.
 * Must be g_free'd after use.
 */
static gchar *sipe_publish_get_category_state_user(struct sipe_core_private *sipe_private)
{
	return sipe_publish_get_category_state(sipe_private, TRUE);
}

static void send_publish_category_initial(struct sipe_core_private *sipe_private)
{
	gchar *pub_device   = sipe_publish_get_category_device(sipe_private);
	gchar *pub_machine;
	gchar *publications;

	sipe_set_initial_status(sipe_private);

	pub_machine  = sipe_publish_get_category_state_machine(sipe_private);
	publications = g_strdup_printf("%s%s",
				       pub_device,
				       pub_machine ? pub_machine : "");
	g_free(pub_device);
	g_free(pub_machine);

	send_presence_publish(sipe_private, publications);
	g_free(publications);
}

static gboolean process_send_presence_category_publish_response(struct sipe_core_private *sipe_private,
								struct sipmsg *msg,
								struct transaction *trans)
{
	const gchar *contenttype = sipmsg_find_header(msg, "Content-Type");

	if (msg->response == 409 && g_str_has_prefix(contenttype, "application/msrtc-fault+xml")) {
		sipe_xml *xml;
		const sipe_xml *node;
		gchar *fault_code;
		GHashTable *faults;
		int index_our;
		gboolean has_device_publication = FALSE;

		xml = sipe_xml_parse(msg->body, msg->bodylen);

		/* test if version mismatch fault */
		fault_code = sipe_xml_data(sipe_xml_child(xml, "Faultcode"));
		if (!sipe_strequal(fault_code, "Client.BadCall.WrongDelta")) {
			SIPE_DEBUG_INFO("process_send_presence_category_publish_response: unsupported fault code:%s returning.", fault_code);
			g_free(fault_code);
			sipe_xml_free(xml);
			return TRUE;
		}
		g_free(fault_code);

		/* accumulating information about faulty versions */
		faults = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		for (node = sipe_xml_child(xml, "details/operation");
		     node;
		     node = sipe_xml_twin(node))
		{
			const gchar *index = sipe_xml_attribute(node, "index");
			const gchar *curVersion = sipe_xml_attribute(node, "curVersion");

			g_hash_table_insert(faults, g_strdup(index), g_strdup(curVersion));
			SIPE_DEBUG_INFO("fault added: index:%s curVersion:%s", index, curVersion);
		}
		sipe_xml_free(xml);

		/* here we are parsing our own request to figure out what publication
		 * referenced here only by index went wrong
		 */
		xml = sipe_xml_parse(trans->msg->body, trans->msg->bodylen);

		/* publication */
		for (node = sipe_xml_child(xml, "publications/publication"),
		     index_our = 1; /* starts with 1 - our first publication */
		     node;
		     node = sipe_xml_twin(node), index_our++)
		{
			gchar *idx = g_strdup_printf("%d", index_our);
			const gchar *curVersion = g_hash_table_lookup(faults, idx);
			const gchar *categoryName = sipe_xml_attribute(node, "categoryName");
			g_free(idx);

			if (sipe_strequal("device", categoryName)) {
				has_device_publication = TRUE;
			}

			if (curVersion) { /* fault exist on this index */
				struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
				const gchar *container = sipe_xml_attribute(node, "container");
				const gchar *instance = sipe_xml_attribute(node, "instance");
				/* key is <category><instance><container> */
				gchar *key = g_strdup_printf("<%s><%s><%s>", categoryName, instance, container);
				GHashTable *category = g_hash_table_lookup(sip->our_publications, categoryName);

				if (category) {
					struct sipe_publication *publication =
						g_hash_table_lookup(category, key);

					SIPE_DEBUG_INFO("key is %s", key);

					if (publication) {
						SIPE_DEBUG_INFO("Updating %s with version %s. Was %d before.",
								key, curVersion, publication->version);
						/* updating publication's version to the correct one */
						publication->version = atoi(curVersion);
					}
				} else {
					/* We somehow lost this category from our publications... */
					struct sipe_publication *publication = g_new0(struct sipe_publication, 1);
					publication->category  = g_strdup(categoryName);
					publication->instance  = atoi(instance);
					publication->container = atoi(container);
					publication->version   = atoi(curVersion);
					category = g_hash_table_new_full(g_str_hash, g_str_equal,
									 g_free, (GDestroyNotify)free_publication);
					g_hash_table_insert(category, g_strdup(key), publication);
					g_hash_table_insert(sip->our_publications, g_strdup(categoryName), category);
					SIPE_DEBUG_INFO("added lost category '%s' key '%s'", categoryName, key);
				}
				g_free(key);
			}
		}
		sipe_xml_free(xml);
		g_hash_table_destroy(faults);

		/* rebublishing with right versions */
		if (has_device_publication) {
			send_publish_category_initial(sipe_private);
		} else {
			send_presence_status(sipe_private, NULL);
		}
	}
	return TRUE;
}

/**
 * Publishes categories.
 * @param uri		(%s) Self URI. Ex.: sip:alice7@boston.local
 * @param publications	(%s) XML publications
 */
#define SIPE_SEND_PRESENCE \
	"<publish xmlns=\"http://schemas.microsoft.com/2006/09/sip/rich-presence\">"\
		"<publications uri=\"%s\">"\
			"%s"\
		"</publications>"\
	"</publish>"

static void send_presence_publish(struct sipe_core_private *sipe_private,
				  const char *publications)
{
	gchar *uri;
	gchar *doc;
	gchar *tmp;
	gchar *hdr;

	uri = sip_uri_self(sipe_private);
	doc = g_strdup_printf(SIPE_SEND_PRESENCE,
		uri,
		publications);

	tmp = get_contact(sipe_private);
	hdr = g_strdup_printf("Contact: %s\r\n"
		"Content-Type: application/msrtc-category-publish+xml\r\n", tmp);

	sip_transport_service(sipe_private,
			      uri,
			      hdr,
			      doc,
			      process_send_presence_category_publish_response);

	g_free(tmp);
	g_free(hdr);
	g_free(uri);
	g_free(doc);
}

/**
 * Publishes self status
 * based on own calendar information.
 */
void sipe_ocs2007_presence_publish(struct sipe_core_private *sipe_private,
				   SIPE_UNUSED_PARAMETER void *unused)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	struct sipe_cal_event* event = NULL;
	gchar *pub_cal_working_hours = NULL;
	gchar *pub_cal_free_busy = NULL;
	gchar *pub_calendar = NULL;
	gchar *pub_calendar2 = NULL;
	gchar *pub_oof_note = NULL;
	const gchar *oof_note;
	time_t oof_start = 0;
	time_t oof_end = 0;

	if (!sip->cal) {
		SIPE_DEBUG_INFO_NOFORMAT("publish_calendar_status_self() no calendar data.");
		return;
	}

	SIPE_DEBUG_INFO_NOFORMAT("publish_calendar_status_self() started.");
	if (sip->cal->cal_events) {
		event = sipe_cal_get_event(sip->cal->cal_events, time(NULL));
	}

	if (!event) {
		SIPE_DEBUG_INFO_NOFORMAT("publish_calendar_status_self: current event is NULL");
	} else {
		char *desc = sipe_cal_event_describe(event);
		SIPE_DEBUG_INFO("publish_calendar_status_self: current event is:\n%s", desc ? desc : "");
		g_free(desc);
	}

	/* Logic
	if OOF
		OOF publish, Busy clean
	ilse if Busy
		OOF clean, Busy publish
	else
		OOF clean, Busy clean
	*/
	if (event && event->cal_status == SIPE_CAL_OOF) {
		pub_calendar  = sipe_publish_get_category_state_calendar(sipe_private, event, sip->cal->email, SIPE_CAL_OOF);
		pub_calendar2 = sipe_publish_get_category_state_calendar(sipe_private, NULL,  sip->cal->email, SIPE_CAL_BUSY);
	} else if (event && event->cal_status == SIPE_CAL_BUSY) {
		pub_calendar  = sipe_publish_get_category_state_calendar(sipe_private, NULL,  sip->cal->email, SIPE_CAL_OOF);
		pub_calendar2 = sipe_publish_get_category_state_calendar(sipe_private, event, sip->cal->email, SIPE_CAL_BUSY);
	} else {
		pub_calendar  = sipe_publish_get_category_state_calendar(sipe_private, NULL,  sip->cal->email, SIPE_CAL_OOF);
		pub_calendar2 = sipe_publish_get_category_state_calendar(sipe_private, NULL,  sip->cal->email, SIPE_CAL_BUSY);
	}

	oof_note = sipe_ews_get_oof_note(sip->cal);
	if (sipe_strequal("Scheduled", sip->cal->oof_state)) {
		oof_start = sip->cal->oof_start;
		oof_end = sip->cal->oof_end;
	}
	pub_oof_note = sipe_publish_get_category_note(sipe_private, oof_note, "OOF", oof_start, oof_end);

	pub_cal_working_hours = sipe_publish_get_category_cal_working_hours(sipe_private);
	pub_cal_free_busy = sipe_publish_get_category_cal_free_busy(sipe_private);

	if (!pub_cal_working_hours && !pub_cal_free_busy && !pub_calendar && !pub_calendar2 && !pub_oof_note) {
		SIPE_DEBUG_INFO_NOFORMAT("publish_calendar_status_self: nothing has changed.");
	} else {
		gchar *publications = g_strdup_printf("%s%s%s%s%s",
				       pub_cal_working_hours ? pub_cal_working_hours : "",
				       pub_cal_free_busy ? pub_cal_free_busy : "",
				       pub_calendar ? pub_calendar : "",
				       pub_calendar2 ? pub_calendar2 : "",
				       pub_oof_note ? pub_oof_note : "");

		send_presence_publish(sipe_private, publications);
		g_free(publications);
	}

	g_free(pub_cal_working_hours);
	g_free(pub_cal_free_busy);
	g_free(pub_calendar);
	g_free(pub_calendar2);
	g_free(pub_oof_note);

	/* repeat scheduling */
	schedule_publish_update(sipe_private, time(NULL));
}

void sipe_ocs2007_category_publish(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	gchar *pub_state = sipe_status_changed_by_user(sipe_private) ?
				sipe_publish_get_category_state_user(sipe_private) :
				sipe_publish_get_category_state_machine(sipe_private);
	gchar *pub_note = sipe_publish_get_category_note(sipe_private,
							 sip->note,
							 sip->is_oof_note ? "OOF" : "personal",
							 0,
							 0);
	gchar *publications;

	if (!pub_state && !pub_note) {
		SIPE_DEBUG_INFO_NOFORMAT("sipe_osc2007_category_publish: nothing has changed. Exiting.");
		return;
	}

	publications = g_strdup_printf("%s%s",
				       pub_state ? pub_state : "",
				       pub_note ? pub_note : "");

	g_free(pub_state);
	g_free(pub_note);

	send_presence_publish(sipe_private, publications);
	g_free(publications);
}

static void sipe_publish_get_cat_state_user_to_clear(SIPE_UNUSED_PARAMETER const char *name,
						     gpointer value,
						     GString* str)
{
	struct sipe_publication *publication = value;

	g_string_append_printf( str,
				SIPE_PUB_XML_PUBLICATION_CLEAR,
				publication->category,
				publication->instance,
				publication->container,
				publication->version,
				"static");
}

void sipe_ocs2007_reset_status(struct sipe_core_private *sipe_private)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	GString* str;
	gchar *publications;

	if (!sip->user_state_publications || g_hash_table_size(sip->user_state_publications) == 0) {
		SIPE_DEBUG_INFO_NOFORMAT("sipe_reset_status: no userState publications, exiting.");
		return;
	}

	str = g_string_new(NULL);
	g_hash_table_foreach(sip->user_state_publications, (GHFunc)sipe_publish_get_cat_state_user_to_clear, str);
	publications = g_string_free(str, FALSE);

	send_presence_publish(sipe_private, publications);
	g_free(publications);
}

/* key is <category><instance><container> */
static gboolean sipe_is_our_publication(struct sipe_core_private *sipe_private,
					const gchar *key)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	GSList *entry;

	/* filling keys for our publications if not yet cached */
	if (!sip->our_publication_keys) {
		guint device_instance 	= sipe_get_pub_instance(sipe_private, SIPE_PUB_DEVICE);
		guint machine_instance 	= sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_MACHINE);
		guint user_instance 	= sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_USER);
		guint calendar_instance	= sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_CALENDAR);
		guint cal_oof_instance	= sipe_get_pub_instance(sipe_private, SIPE_PUB_STATE_CALENDAR_OOF);
		guint cal_data_instance = sipe_get_pub_instance(sipe_private, SIPE_PUB_CALENDAR_DATA);
		guint note_oof_instance = sipe_get_pub_instance(sipe_private, SIPE_PUB_NOTE_OOF);

		SIPE_DEBUG_INFO_NOFORMAT("* Our Publication Instances *");
		SIPE_DEBUG_INFO("\tDevice               : %u\t0x%08X", device_instance, device_instance);
		SIPE_DEBUG_INFO("\tMachine State        : %u\t0x%08X", machine_instance, machine_instance);
		SIPE_DEBUG_INFO("\tUser Stare           : %u\t0x%08X", user_instance, user_instance);
		SIPE_DEBUG_INFO("\tCalendar State       : %u\t0x%08X", calendar_instance, calendar_instance);
		SIPE_DEBUG_INFO("\tCalendar OOF State   : %u\t0x%08X", cal_oof_instance, cal_oof_instance);
		SIPE_DEBUG_INFO("\tCalendar FreeBusy    : %u\t0x%08X", cal_data_instance, cal_data_instance);
		SIPE_DEBUG_INFO("\tOOF Note             : %u\t0x%08X", note_oof_instance, note_oof_instance);
		SIPE_DEBUG_INFO("\tNote                 : %u", 0);
		SIPE_DEBUG_INFO("\tCalendar WorkingHours: %u", 0);

		/* device */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "device", device_instance, 2));

		/* state:machineState */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", machine_instance, 2));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", machine_instance, 3));

		/* state:userState */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", user_instance, 2));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", user_instance, 3));

		/* state:calendarState */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", calendar_instance, 2));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", calendar_instance, 3));

		/* state:calendarState OOF */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", cal_oof_instance, 2));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "state", cal_oof_instance, 3));

		/* note */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "note", 0, 200));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "note", 0, 300));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "note", 0, 400));

		/* note OOF */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "note", note_oof_instance, 200));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "note", note_oof_instance, 300));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "note", note_oof_instance, 400));

		/* calendarData:WorkingHours */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 1));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 100));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 200));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 300));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 400));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", 0, 32000));

		/* calendarData:FreeBusy */
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 1));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 100));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 200));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 300));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 400));
		sip->our_publication_keys = g_slist_append(sip->our_publication_keys,
			g_strdup_printf("<%s><%u><%u>", "calendarData", cal_data_instance, 32000));

		//SIPE_DEBUG_INFO("sipe_is_our_publication: sip->our_publication_keys length=%d",
		//	  sip->our_publication_keys ? (int) g_slist_length(sip->our_publication_keys) : -1);
	}

	//SIPE_DEBUG_INFO("sipe_is_our_publication: key=%s", key);

	entry = sip->our_publication_keys;
	while (entry) {
		//SIPE_DEBUG_INFO("   sipe_is_our_publication: entry->data=%s", entry->data);
		if (sipe_strequal(entry->data, key)) {
			return TRUE;
		}
		entry = entry->next;
	}
	return FALSE;
}

static void sipe_refresh_blocked_status_cb(char *buddy_name,
					   SIPE_UNUSED_PARAMETER struct sipe_buddy *buddy,
					   struct sipe_core_private *sipe_private)
{
	int container_id = sipe_ocs2007_find_access_level(sipe_private, "user", buddy_name, NULL);
	gboolean blocked = (container_id == 32000);
	gboolean blocked_in_blist = sipe_backend_buddy_is_blocked(SIPE_CORE_PUBLIC, buddy_name);

	/* SIPE_DEBUG_INFO("sipe_refresh_blocked_status_cb: buddy_name=%s, blocked=%s, blocked_in_blist=%s",
		buddy_name, blocked ? "T" : "F", blocked_in_blist ? "T" : "F"); */

	if (blocked != blocked_in_blist) {
		sipe_backend_buddy_set_blocked_status(SIPE_CORE_PUBLIC, buddy_name, blocked);
	}
}

static void sipe_refresh_blocked_status(struct sipe_core_private *sipe_private)
{
	g_hash_table_foreach(sipe_private->buddies,
			     (GHFunc) sipe_refresh_blocked_status_cb,
			     sipe_private);
}

/**
  *   When we receive some self (BE) NOTIFY with a new subscriber
  *   we sends a setSubscribers request to him [SIP-PRES] 4.8
  *
  */
void sipe_ocs2007_process_roaming_self(struct sipe_core_private *sipe_private,
				       struct sipmsg *msg)
{
	struct sipe_account_data *sip = SIPE_ACCOUNT_DATA_PRIVATE;
	gchar *contact;
	gchar *to;
	sipe_xml *xml;
	const sipe_xml *node;
	const sipe_xml *node2;
        char *display_name = NULL;
        char *uri;
	GSList *category_names = NULL;
	int aggreg_avail = 0;
	gboolean do_update_status = FALSE;
	gboolean has_note_cleaned = FALSE;
	GHashTable *devices;

	SIPE_DEBUG_INFO_NOFORMAT("sipe_ocs2007_process_roaming_self");

	xml = sipe_xml_parse(msg->body, msg->bodylen);
	if (!xml) return;

	contact = get_contact(sipe_private);
	to = sip_uri_self(sipe_private);

	/* categories */
	/* set list of categories participating in this XML */
	for (node = sipe_xml_child(xml, "categories/category"); node; node = sipe_xml_twin(node)) {
		const gchar *name = sipe_xml_attribute(node, "name");
		category_names = slist_insert_unique_sorted(category_names, (gchar *)name, (GCompareFunc)strcmp);
	}
	SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: category_names length=%d",
			category_names ? (int) g_slist_length(category_names) : -1);
	/* drop category information */
	if (category_names) {
		GSList *entry = category_names;
		while (entry) {
			GHashTable *cat_publications;
			const gchar *category = entry->data;
			entry = entry->next;
			SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: dropping category: %s", category);
			cat_publications = g_hash_table_lookup(sip->our_publications, category);
			if (cat_publications) {
				g_hash_table_remove(sip->our_publications, category);
				SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: dropped category: %s", category);
			}
		}
	}
	g_slist_free(category_names);

	/* filling our categories reflected in roaming data */
	devices = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, NULL);
	for (node = sipe_xml_child(xml, "categories/category"); node; node = sipe_xml_twin(node)) {
		const char *tmp;
		const gchar *name = sipe_xml_attribute(node, "name");
		guint container = sipe_xml_int_attribute(node, "container", -1);
		guint instance  = sipe_xml_int_attribute(node, "instance", -1);
		guint version   = sipe_xml_int_attribute(node, "version", 0);
		time_t publish_time = (tmp = sipe_xml_attribute(node, "publishTime")) ?
			sipe_utils_str_to_time(tmp) : 0;
		gchar *key;
		GHashTable *cat_publications = g_hash_table_lookup(sip->our_publications, name);

		/* Ex. clear note: <category name="note"/> */
		if (container == (guint)-1) {
			g_free(sip->note);
			sip->note = NULL;
			do_update_status = TRUE;
			continue;
		}

		/* Ex. clear note: <category name="note" container="200"/> */
		if (instance == (guint)-1) {
			if (container == 200) {
				g_free(sip->note);
				sip->note = NULL;
				do_update_status = TRUE;
			}
			SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: removing publications for: %s/%u", name, container);
			sipe_remove_category_container_publications(
				sip->our_publications, name, container);
			continue;
		}

		/* key is <category><instance><container> */
		key = g_strdup_printf("<%s><%u><%u>", name, instance, container);
		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: key=%s version=%d", key, version);

		/* capture all userState publication for later clean up if required */
		if (sipe_strequal(name, "state") && (container == 2 || container == 3)) {
			const sipe_xml *xn_state = sipe_xml_child(node, "state");

			if (xn_state && sipe_strequal(sipe_xml_attribute(xn_state, "type"), "userState")) {
				struct sipe_publication *publication = g_new0(struct sipe_publication, 1);
				publication->category  = g_strdup(name);
				publication->instance  = instance;
				publication->container = container;
				publication->version   = version;

				if (!sip->user_state_publications) {
					sip->user_state_publications = g_hash_table_new_full(
									g_str_hash, g_str_equal,
									g_free,	(GDestroyNotify)free_publication);
				}
				g_hash_table_insert(sip->user_state_publications, g_strdup(key), publication);
				SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: added to user_state_publications key=%s version=%d",
						key, version);
			}
		}

		/* count each client instance only once */
		if (sipe_strequal(name, "device"))
			g_hash_table_replace(devices, g_strdup_printf("%u", instance), NULL);

		if (sipe_is_our_publication(sipe_private, key)) {
			struct sipe_publication *publication = g_new0(struct sipe_publication, 1);

			publication->category = g_strdup(name);
			publication->instance  = instance;
			publication->container = container;
			publication->version   = version;

			/* filling publication->availability */
			if (sipe_strequal(name, "state")) {
				const sipe_xml *xn_state = sipe_xml_child(node, "state");
				const sipe_xml *xn_avail = sipe_xml_child(xn_state, "availability");

				if (xn_avail) {
					gchar *avail_str = sipe_xml_data(xn_avail);
					if (avail_str) {
						publication->availability = atoi(avail_str);
					}
					g_free(avail_str);
				}
				/* for calendarState */
				if (xn_state && sipe_strequal(sipe_xml_attribute(xn_state, "type"), "calendarState")) {
					const sipe_xml *xn_activity = sipe_xml_child(xn_state, "activity");
					struct sipe_cal_event *event = g_new0(struct sipe_cal_event, 1);

					event->start_time = sipe_utils_str_to_time(sipe_xml_attribute(xn_state, "startTime"));
					if (xn_activity) {
						if (sipe_strequal(sipe_xml_attribute(xn_activity, "token"),
								  sipe_activity_to_token(SIPE_ACTIVITY_IN_MEETING)))
						{
							event->is_meeting = TRUE;
						}
					}
					event->subject = sipe_xml_data(sipe_xml_child(xn_state, "meetingSubject"));
					event->location = sipe_xml_data(sipe_xml_child(xn_state, "meetingLocation"));

					publication->cal_event_hash = sipe_cal_event_hash(event);
					SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: hash=%s",
							publication->cal_event_hash);
					sipe_cal_event_free(event);
				}
			}
			/* filling publication->note */
			if (sipe_strequal(name, "note")) {
				const sipe_xml *xn_body = sipe_xml_child(node, "note/body");

				if (!has_note_cleaned) {
					has_note_cleaned = TRUE;

					g_free(sip->note);
					sip->note = NULL;
					sip->note_since = publish_time;

					do_update_status = TRUE;
				}

				g_free(publication->note);
				publication->note = NULL;
				if (xn_body) {
					char *tmp;

					publication->note = g_markup_escape_text((tmp = sipe_xml_data(xn_body)), -1);
					g_free(tmp);
					if (publish_time >= sip->note_since) {
						g_free(sip->note);
						sip->note = g_strdup(publication->note);
						sip->note_since = publish_time;
						sip->is_oof_note = sipe_strequal(sipe_xml_attribute(xn_body, "type"), "OOF");

						do_update_status = TRUE;
					}
				}
			}

			/* filling publication->fb_start_str, free_busy_base64, working_hours_xml_str */
			if (sipe_strequal(name, "calendarData") && (publication->container == 300)) {
				const sipe_xml *xn_free_busy = sipe_xml_child(node, "calendarData/freeBusy");
				const sipe_xml *xn_working_hours = sipe_xml_child(node, "calendarData/WorkingHours");
				if (xn_free_busy) {
					publication->fb_start_str = g_strdup(sipe_xml_attribute(xn_free_busy, "startTime"));
					publication->free_busy_base64 = sipe_xml_data(xn_free_busy);
				}
				if (xn_working_hours) {
					publication->working_hours_xml_str = sipe_xml_stringify(xn_working_hours);
				}
			}

			if (!cat_publications) {
				cat_publications = g_hash_table_new_full(
							g_str_hash, g_str_equal,
							g_free,	(GDestroyNotify)free_publication);
				g_hash_table_insert(sip->our_publications, g_strdup(name), cat_publications);
				SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: added GHashTable cat=%s", name);
			}
			g_hash_table_insert(cat_publications, g_strdup(key), publication);
			SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: added key=%s version=%d", key, version);
		}
		g_free(key);

		/* aggregateState (not an our publication) from 2-nd container */
		if (sipe_strequal(name, "state") && container == 2) {
			const sipe_xml *xn_state = sipe_xml_child(node, "state");

			if (xn_state && sipe_strequal(sipe_xml_attribute(xn_state, "type"), "aggregateState")) {
				const sipe_xml *xn_avail = sipe_xml_child(xn_state, "availability");

				if (xn_avail) {
					gchar *avail_str = sipe_xml_data(xn_avail);
					if (avail_str) {
						aggreg_avail = atoi(avail_str);
					}
					g_free(avail_str);
				}

				do_update_status = TRUE;
			}
		}

		/* userProperties published by server from AD */
		if (!sip->csta && sipe_strequal(name, "userProperties")) {
			const sipe_xml *line;
			/* line, for Remote Call Control (RCC) */
			for (line = sipe_xml_child(node, "userProperties/lines/line"); line; line = sipe_xml_twin(line)) {
				const gchar *line_server = sipe_xml_attribute(line, "lineServer");
				const gchar *line_type = sipe_xml_attribute(line, "lineType");
				gchar *line_uri;

				if (!line_server || !(sipe_strequal(line_type, "Rcc") || sipe_strequal(line_type, "Dual"))) continue;

				line_uri = sipe_xml_data(line);
				if (line_uri) {
					SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: line_uri=%s server=%s", line_uri, line_server);
					sip_csta_open(sipe_private, line_uri, line_server);
				}
				g_free(line_uri);

				break;
			}
		}
	}
	SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: sip->our_publications size=%d",
			sip->our_publications ? (int) g_hash_table_size(sip->our_publications) : -1);

	/* active clients for user account */
	if (g_hash_table_size(devices) > 1) {
		SIPE_CORE_PRIVATE_FLAG_SET(MPOP);
		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: multiple clients detected (%d)",
				g_hash_table_size(devices));
	} else {
		SIPE_CORE_PRIVATE_FLAG_UNSET(MPOP);
		SIPE_DEBUG_INFO_NOFORMAT("sipe_ocs2007_process_roaming_self: single client detected");
	}
	g_hash_table_destroy(devices);

	/* containers */
	for (node = sipe_xml_child(xml, "containers/container"); node; node = sipe_xml_twin(node)) {
		guint id = sipe_xml_int_attribute(node, "id", 0);
		struct sipe_container *container = sipe_find_container(sipe_private, id);

		if (container) {
			sip->containers = g_slist_remove(sip->containers, container);
			SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: removed existing container id=%d v%d", container->id, container->version);
			sipe_ocs2007_free_container(container);
		}
		container = g_new0(struct sipe_container, 1);
		container->id = id;
		container->version = sipe_xml_int_attribute(node, "version", 0);
		sip->containers = g_slist_append(sip->containers, container);
		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: added container id=%d v%d", container->id, container->version);

		for (node2 = sipe_xml_child(node, "member"); node2; node2 = sipe_xml_twin(node2)) {
			struct sipe_container_member *member = g_new0(struct sipe_container_member, 1);
			member->type = g_strdup(sipe_xml_attribute(node2, "type"));
			member->value = g_strdup(sipe_xml_attribute(node2, "value"));
			container->members = g_slist_append(container->members, member);
			SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: added container member type=%s value=%s",
					member->type, member->value ? member->value : "");
		}
	}

	SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: sip->access_level_set=%s", sip->access_level_set ? "TRUE" : "FALSE");
	if (!sip->access_level_set && sipe_xml_child(xml, "containers")) {
		char *container_xmls = NULL;
		int sameEnterpriseAL = sipe_ocs2007_find_access_level(sipe_private, "sameEnterprise", NULL, NULL);
		int federatedAL      = sipe_ocs2007_find_access_level(sipe_private, "federated", NULL, NULL);

		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: sameEnterpriseAL=%d", sameEnterpriseAL);
		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: federatedAL=%d", federatedAL);
		/* initial set-up to let counterparties see your status */
		if (sameEnterpriseAL < 0) {
			struct sipe_container *container = sipe_find_container(sipe_private, 200);
			guint version = container ? container->version : 0;
			sipe_send_container_members_prepare(200, version, "add", "sameEnterprise", NULL, &container_xmls);
		}
		if (federatedAL < 0) {
			struct sipe_container *container = sipe_find_container(sipe_private, 100);
			guint version = container ? container->version : 0;
			sipe_send_container_members_prepare(100, version, "add", "federated", NULL, &container_xmls);
		}
		sip->access_level_set = TRUE;

		if (container_xmls) {
			sipe_send_set_container_members(sipe_private, container_xmls);
		}
		g_free(container_xmls);
	}

	/* Refresh contacts' blocked status */
	sipe_refresh_blocked_status(sipe_private);

	/* subscribers */
	for (node = sipe_xml_child(xml, "subscribers/subscriber"); node; node = sipe_xml_twin(node)) {
		const char *user;
		const char *acknowledged;
		gchar *hdr;
		gchar *body;

		user = sipe_xml_attribute(node, "user"); /* without 'sip:' prefix */
		if (!user) continue;
		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: user %s", user);
		display_name = g_strdup(sipe_xml_attribute(node, "displayName"));
		uri = sip_uri_from_name(user);

		sipe_buddy_update_property(sipe_private, uri, SIPE_BUDDY_INFO_DISPLAY_NAME, display_name);

	        acknowledged= sipe_xml_attribute(node, "acknowledged");
		if(sipe_strcase_equal(acknowledged,"false")){
                        SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: user added you %s", user);
			if (!sipe_backend_buddy_find(SIPE_CORE_PUBLIC, uri, NULL)) {
				sipe_backend_buddy_request_add(SIPE_CORE_PUBLIC, uri, display_name);
			}

		        hdr = g_strdup_printf(
				      "Contact: %s\r\n"
				      "Content-Type: application/msrtc-presence-setsubscriber+xml\r\n", contact);

		        body = g_strdup_printf(
				       "<setSubscribers xmlns=\"http://schemas.microsoft.com/2006/09/sip/presence-subscribers\">"
				       "<subscriber user=\"%s\" acknowledged=\"true\"/>"
				       "</setSubscribers>", user);

		        sip_transport_service(sipe_private,
					      to,
					      hdr,
					      body,
					      NULL);
		        g_free(body);
		        g_free(hdr);
                }
		g_free(display_name);
		g_free(uri);
	}

	g_free(contact);
	sipe_xml_free(xml);

	/* Publish initial state if not yet.
	 * Assuming this happens on initial responce to subscription to roaming-self
	 * so we've already updated our roaming data in full.
	 * Only for 2007+
	 */
	if (!sip->initial_state_published) {
		send_publish_category_initial(sipe_private);
		sipe_groupchat_init(sipe_private);
		sip->initial_state_published = TRUE;
		/* dalayed run */
		sipe_cal_delayed_calendar_update(sipe_private);
		do_update_status = FALSE;
	} else if (aggreg_avail) {

		if (aggreg_avail && aggreg_avail < 18000) { /* not offline */
			g_free(sip->status);
			sip->status = g_strdup(sipe_get_status_by_availability(aggreg_avail, NULL));
		} else {
			sipe_set_invisible_status(sipe_private); /* not not let offline status switch us off */
		}
	}

	if (do_update_status) {
		SIPE_DEBUG_INFO("sipe_ocs2007_process_roaming_self: switch to '%s' for the account", sip->status);
		sipe_status_and_note(sipe_private, sip->status);
	}

	g_free(to);
}

/*
  Local Variables:
  mode: c
  c-file-style: "bsd"
  indent-tabs-mode: t
  tab-width: 8
  End:
*/