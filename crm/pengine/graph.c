/* $Id: graph.c,v 1.81 2006/04/10 12:17:38 andrew Exp $ */
/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <portability.h>

#include <sys/param.h>
#include <crm/crm.h>
#include <crm/cib.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/msg.h>

#include <glib.h>

#include <pengine.h>
#include <pe_utils.h>

gboolean update_action(action_t *action);

gboolean
update_action_states(GListPtr actions)
{
	crm_debug_2("Updating %d actions", g_list_length(actions));
	slist_iter(
		action, action_t, actions, lpc,

		update_action(action);
		);

	return TRUE;
}

#define UPDATE_THEM 1

gboolean
update_action(action_t *action)
{
	gboolean change = FALSE;
	enum action_tasks task = no_action;
	
	crm_debug_3("Processing action %s: %s",
		    action->uuid, action->optional?"optional":"required");

	slist_iter(
		other, action_wrapper_t, action->actions_before, lpc,
		crm_debug_3("\tChecking action %s: %s/%s",
			    other->action->uuid, ordering_type2text(other->type),
			    other->action->optional?"optional":"required");

		if(other->type == pe_ordering_restart
		   && action->rsc->role > RSC_ROLE_STOPPED) {
			crm_debug_3("Upgrading %s constraint to %s",
				    ordering_type2text(other->type),
				    ordering_type2text(pe_ordering_manditory));
			other->type = pe_ordering_manditory;
		}
		
		if(other->type != pe_ordering_manditory) {
			crm_debug_3("\t  Ignoring: %s",
				    ordering_type2text(other->type));
			continue;
			
		} else if(action->optional || other->action->optional == FALSE){
			crm_debug_3("\t  Ignoring: %s/%s",
				    other->action->optional?"-":"they are not optional",
				    action->optional?"we are optional":"-");
			continue;
			
		} else if(safe_str_eq(other->action->task, CRMD_ACTION_START)) {
			const char *interval = g_hash_table_lookup(
				action->extra, "interval");
			int interval_i = 0;
			if(interval != NULL) {
				interval_i = crm_parse_int(interval, NULL);
				if(interval_i > 0) {
					crm_debug_3("Ignoring: start + recurring");
					continue;
				}
			}
		}

		other->action->optional = FALSE;
		crm_debug_2("* Marking action %s manditory because of %s",
			    other->action->uuid, action->uuid);
		update_action(other->action);
		);

	slist_iter(
		other, action_wrapper_t, action->actions_after, lpc,
		
		if(action->pseudo == FALSE && action->runnable == FALSE) {
			if(other->action->runnable == FALSE) {
				crm_debug_2("Action %s already un-runnable",
					  other->action->uuid);
			} else if(action->optional == FALSE) {
				other->action->runnable = FALSE;
				crm_debug_2("Marking action %s un-runnable"
					  " because of %s",
					  other->action->uuid, action->uuid);
				update_action(other->action);
			}
		}

		crm_debug_3("\t(Recover) Checking action %s: %s/%s",
			    other->action->uuid, ordering_type2text(other->type),
			    other->action->optional?"optional":"required");

		if(other->action->rsc == NULL) {
			continue;
			
		} else if(other->type == pe_ordering_recover) {
			if(other->action->rsc->restart_type != pe_restart_restart) {
				crm_debug_3("\t  Ignoring: restart type %d",
					    other->action->rsc->restart_type);
				continue;
			}
			
		} else if(other->type == pe_ordering_restart) {
		} else if(other->type == pe_ordering_postnotify) {
			CRM_CHECK(action->rsc == other->action->rsc, continue);

		} else {
			crm_debug_3("\t  Ignoring: ordering %s",
				    ordering_type2text(other->type));
			continue;
		}
		
		if(other->action->optional == FALSE || action->optional) {
			crm_debug_3("\t  Ignoring: %s/%s",
				    action->optional?"we are optional":"-",
				    other->action->optional?"-":"they are not optional");
			continue;
		}

		task = text2task(action->task);
		switch(task) {
			case stop_rsc:
			case stopped_rsc:
				crm_debug_3("\t  Ignoring: action %s",
					    action->uuid);
				break;
			case start_rsc:
			case started_rsc:
				crm_debug_2("* (Recover) Marking action %s"
					    " manditory because of %s",
					    other->action->uuid, action->uuid);
				other->action->optional = FALSE; 
				update_action(other->action);
				break;
			default:
				crm_debug_3("\t  Ignoring: action %s",
					    action->uuid);
				break;
		}
		);

	if(change) {
		update_action(action);
	}

	crm_debug_3("Action %s: %s", action->uuid, change?"update":"untouched");
	return change;
}


gboolean
shutdown_constraints(
	node_t *node, action_t *shutdown_op, pe_working_set_t *data_set)
{
	/* add the stop to the before lists so it counts as a pre-req
	 * for the shutdown
	 */
	slist_iter(
		rsc, resource_t, node->details->running_rsc, lpc,

		if(rsc->is_managed == FALSE) {
			continue;
		}
		
		custom_action_order(
			rsc, stop_key(rsc), NULL,
			NULL, crm_strdup(CRM_OP_SHUTDOWN), shutdown_op,
			pe_ordering_manditory, data_set);

		);	

	return TRUE;	
}

gboolean
stonith_constraints(node_t *node,
		    action_t *stonith_op, action_t *shutdown_op,
		    pe_working_set_t *data_set)
{
	GListPtr action_list = NULL;
	
	if(shutdown_op != NULL && stonith_op != NULL) {
		/* stop everything we can via shutdown_constraints() and then
		 *   shoot the node... the shutdown has been superceeded
		 */
		shutdown_op->pseudo = TRUE;
		shutdown_op->runnable = TRUE;

		/* shutdown before stonith */
		/* Give any resources a chance to shutdown normally */
		crm_debug_4("Adding shutdown (%d) as an input to stonith (%d)",
			  shutdown_op->id, stonith_op->id);
		
		custom_action_order(
			NULL, crm_strdup(CRM_OP_SHUTDOWN), shutdown_op,
			NULL, crm_strdup(CRM_OP_FENCE), stonith_op,
			pe_ordering_manditory, data_set);
		
	}

	/*
	 * Make sure the stonith OP occurs before we start any shared resources
	 */
	if(stonith_op != NULL) {
		slist_iter(
			rsc, resource_t, data_set->resources, lpc,
			rsc->fns->stonith_ordering(rsc, stonith_op, data_set);
			);
	}
	
	/* add the stonith OP as a stop pre-req and the mark the stop
	 * as a pseudo op - since its now redundant
	 */
	slist_iter(
		rsc, resource_t, node->details->running_rsc, lpc,

		if(rsc->is_managed == FALSE) {
			crm_debug_2("Skipping fencing constraints for unmanaged resource: %s", rsc->id);
			
		} else if(stonith_op != NULL) {
			char *key = stop_key(rsc);
			action_list = find_actions(rsc->actions, key, node);
			crm_free(key);
			
			slist_iter(
				action, action_t, action_list, lpc2,
				if(node->details->online == FALSE
				   || rsc->failed) {
					crm_info("Stop of failed resource %s is"
						 " implict after %s is fenced",
						 rsc->id, node->details->uname);
					/* the stop would never complete and is
					 * now implied by the stonith operation
					 */
					action->pseudo = TRUE;
					action->runnable = TRUE;
					custom_action_order(
						NULL, crm_strdup(CRM_OP_FENCE),stonith_op,
						rsc, start_key(rsc), NULL,
						pe_ordering_manditory, data_set);
				} else {
					crm_info("Moving healthy resource %s"
						 " off %s before fencing",
						 rsc->id, node->details->uname);
					
					/* stop healthy resources before the
					 * stonith op
					 */
					custom_action_order(
						rsc, stop_key(rsc), NULL,
						NULL,crm_strdup(CRM_OP_FENCE),stonith_op,
						pe_ordering_manditory, data_set);
				}
				);

			key = demote_key(rsc);
			action_list = find_actions(rsc->actions, key, node);
			crm_free(key);
			
			slist_iter(
				action, action_t, action_list, lpc2,
				if(node->details->online == FALSE || rsc->failed) {
					crm_info("Demote of failed resource %s is"
						 " implict after %s is fenced",
						 rsc->id, node->details->uname);
					/* the stop would never complete and is
					 * now implied by the stonith operation
					 */
					action->pseudo = TRUE;
					action->runnable = TRUE;
					custom_action_order(
						NULL, crm_strdup(CRM_OP_FENCE),stonith_op,
						rsc, demote_key(rsc), NULL,
						pe_ordering_manditory, data_set);
				}
				);

/* 			crm_debug_4("Adding stonith (%d) as an input to stop", */
/* 				  stonith_op->id); */
			
/* 		} else if((rsc->unclean || node->details->unclean) */
/* 			  && rsc->stopfail_type == pesf_block) { */
			
/* 			/\* depend on the stop action which will fail *\/ */
/* 			pe_err("SHARED RESOURCE %s WILL REMAIN BLOCKED" */
/* 				 " ON NODE %s UNTIL %s", */
/* 				rsc->id, node->details->uname, */
/* 				data_set->stonith_enabled?"QUORUM RETURNS":"CLEANED UP MANUALLY"); */
/* 			continue; */
			
/* 		} else if((rsc->unclean || node->details->unclean) */
/* 			  && rsc->stopfail_type == pesf_ignore) { */
/* 			/\* nothing to do here *\/ */
/* 			pe_err("SHARED RESOURCE %s IS NOT PROTECTED", rsc->id); */
/* 			continue; */
		}
		);
	
	return TRUE;
}

static void dup_attr(gpointer key, gpointer value, gpointer user_data)
{
	g_hash_table_replace(user_data, crm_strdup(key), crm_strdup(value));
}

crm_data_t *
action2xml(action_t *action, gboolean as_input)
{
	gboolean needs_node_info = TRUE;
	crm_data_t * action_xml = NULL;
	crm_data_t * args_xml = NULL;
	char *action_id_s = NULL;
	
	if(action == NULL) {
		return NULL;
	}

	crm_debug_4("Dumping action %d as XML", action->id);
	if(safe_str_eq(action->task, CRM_OP_FENCE)) {
		action_xml = create_xml_node(NULL, XML_GRAPH_TAG_CRM_EVENT);
/* 		needs_node_info = FALSE; */
		
	} else if(safe_str_eq(action->task, CRM_OP_SHUTDOWN)) {
		action_xml = create_xml_node(NULL, XML_GRAPH_TAG_CRM_EVENT);

	} else if(safe_str_eq(action->task, CRM_OP_LRM_REFRESH)) {
		action_xml = create_xml_node(NULL, XML_GRAPH_TAG_CRM_EVENT);

/* 	} else if(safe_str_eq(action->task, CRMD_ACTION_PROBED)) { */
/* 		action_xml = create_xml_node(NULL, XML_GRAPH_TAG_CRM_EVENT); */

	} else if(action->pseudo) {
		action_xml = create_xml_node(NULL, XML_GRAPH_TAG_PSEUDO_EVENT);
		needs_node_info = FALSE;

	} else {
		action_xml = create_xml_node(NULL, XML_GRAPH_TAG_RSC_OP);
	}

	action_id_s = crm_itoa(action->id);
	crm_xml_add(action_xml, XML_ATTR_ID, action_id_s);
	crm_free(action_id_s);
	
	if(action->rsc != NULL) {
		crm_xml_add(action_xml, XML_LRM_ATTR_RSCID,
			    action->rsc->graph_name);
	}
	crm_xml_add(action_xml, XML_LRM_ATTR_TASK, action->task);
	crm_xml_add(action_xml, XML_LRM_ATTR_TASK_KEY, action->uuid);

	if(needs_node_info && action->node != NULL) {
		crm_xml_add(action_xml, XML_LRM_ATTR_TARGET,
			    action->node->details->uname);

		crm_xml_add(action_xml, XML_LRM_ATTR_TARGET_UUID,
			    action->node->details->id);		
	}

	if(action->failure_is_fatal == FALSE) {
		g_hash_table_insert(
			action->extra, crm_strdup(XML_ATTR_TE_ALLOWFAIL),
			crm_strdup(XML_BOOLEAN_TRUE));
	}
	
	if(as_input) {
		return action_xml;
	}

	if(action->notify_keys != NULL) {
		g_hash_table_foreach(
			action->notify_keys, dup_attr, action->extra);
	}
	if(action->rsc != NULL && action->pseudo == FALSE) {
		crm_data_t *rsc_xml = create_xml_node(
			action_xml, crm_element_name(action->rsc->xml));

		copy_in_properties(rsc_xml, action->rsc->xml);
		crm_xml_add(rsc_xml, XML_ATTR_ID, action->rsc->graph_name);
		
		args_xml = create_xml_node(action_xml, XML_TAG_ATTRS);
		g_hash_table_foreach(action->extra, hash2field, args_xml);
		
		g_hash_table_foreach(
			action->rsc->parameters, hash2field, args_xml);

	} else {
		args_xml = create_xml_node(action_xml, XML_TAG_ATTRS);
		g_hash_table_foreach(action->extra, hash2field, args_xml);
	}
	crm_log_xml_debug_2(action_xml, "dumped action");
	
	return action_xml;
}

static gboolean
should_dump_action(action_t *action) 
{
	const char * interval = NULL;

	CRM_CHECK(action != NULL, return FALSE);

	interval = g_hash_table_lookup(action->extra, "interval");
	if(action->optional) {
		crm_debug_5("action %d was optional", action->id);
		return FALSE;

	} else if(action->pseudo == FALSE && action->runnable == FALSE) {
		crm_debug_5("action %d was not runnable", action->id);
		return FALSE;

	} else if(action->dumped) {
		crm_debug_5("action %d was already dumped", action->id);
		return FALSE;

	} else if(action->rsc != NULL
		  && action->rsc->is_managed == FALSE) {

		/* make sure probes go through */
		if(safe_str_neq(action->task, CRMD_ACTION_STATUS)) {
			pe_warn("action %d (%s) was for an unmanaged resource (%s)",
				action->id, action->uuid, action->rsc->id);
			return FALSE;
		}
		
		if(interval != NULL && safe_str_neq(interval, "0")) {
			pe_warn("action %d (%s) was for an unmanaged resource (%s)",
				action->id, action->uuid, action->rsc->id);
			return FALSE;
		}
	}
	
	if(action->pseudo
	   || safe_str_eq(action->task,  CRM_OP_FENCE)
	   || safe_str_eq(action->task,  CRM_OP_SHUTDOWN)) {
		/* skip the next checks */
		return TRUE;
	}

	if(action->node == NULL) {
		pe_err("action %d (%s) was not allocated",
		       action->id, action->uuid);
		log_action(LOG_DEBUG, "Unallocated action", action, FALSE);
		return FALSE;
		
	} else if(action->node->details->online == FALSE) {
		pe_err("action %d was (%s) scheduled for offline node",
		       action->id, action->uuid);
		log_action(LOG_DEBUG, "Action for offline node", action, FALSE);
		return FALSE;
#if 0
		/* but this would also affect resources that can be safely
		 *  migrated before a fencing op
		 */
	} else if(action->node->details->unclean == FALSE) {
		pe_err("action %d was (%s) scheduled for unclean node",
		       action->id, action->uuid);
		log_action(LOG_DEBUG, "Action for unclean node", action, FALSE);
		return FALSE;
#endif
	}
	return TRUE;
}


void
graph_element_from_action(action_t *action, pe_working_set_t *data_set)
{
	int last_action = -1;
	int synapse_priority = 0;
	crm_data_t * syn = NULL;
	crm_data_t * set = NULL;
	crm_data_t * in  = NULL;
	crm_data_t * input = NULL;
	crm_data_t * xml_action = NULL;

	if(should_dump_action(action) == FALSE) {
		return;
	}
	
	action->dumped = TRUE;
	
	syn = create_xml_node(data_set->graph, "synapse");
	set = create_xml_node(syn, "action_set");
	in  = create_xml_node(syn, "inputs");

	crm_xml_add_int(syn, XML_ATTR_ID, data_set->num_synapse);
	data_set->num_synapse++;

	if(action->rsc != NULL) {
		synapse_priority = action->rsc->priority;
	}

	if(synapse_priority > 0) {
		crm_xml_add_int(syn, XML_CIB_ATTR_PRIORITY, synapse_priority);
	}
	
	xml_action = action2xml(action, FALSE);
	add_node_copy(set, xml_action);
	free_xml(xml_action);

	action->actions_before = g_list_sort(
		action->actions_before, sort_action_id);
	
	slist_iter(wrapper,action_wrapper_t,action->actions_before,lpc,

		   if(last_action == wrapper->action->id) {
			   crm_debug_2("Input (%d) %s duplicated",
				       wrapper->action->id,
				       wrapper->action->uuid);
			   continue;
			   
		   } else if(wrapper->action->optional == TRUE) {
			   crm_debug_2("Input (%d) %s optional",
				       wrapper->action->id,
				       wrapper->action->uuid);
			   continue;
		   }

		   CRM_CHECK(last_action < wrapper->action->id, ;);
		   last_action = wrapper->action->id;
		   input = create_xml_node(in, "trigger");
		   
		   xml_action = action2xml(wrapper->action, TRUE);
		   add_node_copy(input, xml_action);
		   free_xml(xml_action);
		   
		);
}
