/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */
 
// *****************************************************************************
//
// Minimal setup for HFP Hands-Free (HF) unit (!! UNDER DEVELOPMENT !!)
//
// *****************************************************************************

#include "btstack-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <btstack/hci_cmds.h>
#include <btstack/run_loop.h>

#include "hci.h"
#include "btstack_memory.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "sdp_query_rfcomm.h"
#include "sdp.h"
#include "debug.h"
#include "hfp.h"
#include "hfp_hf.h"


static const char default_hfp_hf_service_name[] = "Hands-Free unit";
static uint16_t hfp_supported_features = HFP_DEFAULT_HF_SUPPORTED_FEATURES;
static uint8_t hfp_codecs_nr = 0;
static uint8_t hfp_codecs[HFP_MAX_NUM_CODECS];

static uint8_t hfp_indicators_nr = 0;
static uint8_t hfp_indicators[HFP_MAX_NUM_HF_INDICATORS];
static uint8_t hfp_indicators_status;

static hfp_callback_t hfp_callback;

void hfp_hf_register_packet_handler(hfp_callback_t callback){
    hfp_callback = callback;
    if (callback == NULL){
        log_error("hfp_hf_register_packet_handler called with NULL callback");
        return;
    }
    hfp_callback = callback;
}


static int has_codec_negotiation_feature(hfp_connection_t * connection){
    int hf = get_bit(hfp_supported_features, HFP_HFSF_CODEC_NEGOTIATION);
    int ag = get_bit(connection->remote_supported_features, HFP_AGSF_CODEC_NEGOTIATION);
    return hf && ag;
}

static int has_call_waiting_and_3way_calling_feature(hfp_connection_t * connection){
    int hf = get_bit(hfp_supported_features, HFP_HFSF_THREE_WAY_CALLING);
    int ag = get_bit(connection->remote_supported_features, HFP_AGSF_THREE_WAY_CALLING);
    return hf && ag;
}


static int has_hf_indicators_feature(hfp_connection_t * connection){
    int hf = get_bit(hfp_supported_features, HFP_HFSF_HF_INDICATORS);
    int ag = get_bit(connection->remote_supported_features, HFP_AGSF_HF_INDICATORS);
    return hf && ag;
}

static void packet_handler(void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

void hfp_hf_create_sdp_record(uint8_t * service, int rfcomm_channel_nr, const char * name, uint16_t supported_features){
    if (!name){
        name = default_hfp_hf_service_name;
    }
    hfp_create_sdp_record(service, SDP_Handsfree, rfcomm_channel_nr, name, supported_features);
}


int hfp_hs_exchange_supported_features_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s=%d\r\n", HFP_SUPPORTED_FEATURES, hfp_supported_features);
    // printf("exchange_supported_features %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_retrieve_codec_cmd(uint16_t cid){
    char buffer[30];
    int offset = snprintf(buffer, sizeof(buffer), "AT%s=", HFP_AVAILABLE_CODECS);
    offset += join(buffer+offset, sizeof(buffer)-offset, hfp_codecs, hfp_codecs_nr);
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_retrieve_indicators_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s=?\r\n", HFP_INDICATOR);
    // printf("retrieve_indicators %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_retrieve_indicators_status_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s?\r\n", HFP_INDICATOR);
    // printf("retrieve_indicators_status %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_activate_status_update_for_all_ag_indicators_cmd(uint16_t cid, uint8_t activate){
    char buffer[20];
    sprintf(buffer, "AT%s=3,0,0,%d\r\n", HFP_ENABLE_STATUS_UPDATE_FOR_AG_INDICATORS, activate);
    // printf("toggle_indicator_status_update %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_activate_status_update_for_ag_indicator_cmd(uint16_t cid, uint32_t indicators_status, int indicators_nr){
    char buffer[50];
    int offset = snprintf(buffer, sizeof(buffer), "AT%s=", HFP_UPDATE_ENABLE_STATUS_FOR_INDIVIDUAL_AG_INDICATORS);
    offset += join_bitmap(buffer+offset, sizeof(buffer)-offset, indicators_status, indicators_nr);
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_retrieve_can_hold_call_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s=?\r\n", HFP_SUPPORT_CALL_HOLD_AND_MULTIPARTY_SERVICES);
    // printf("retrieve_can_hold_call %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_list_supported_generic_status_indicators_cmd(uint16_t cid){
    char buffer[30];
    int offset = snprintf(buffer, sizeof(buffer), "AT%s=", HFP_GENERIC_STATUS_INDICATOR);
    offset += join(buffer+offset, sizeof(buffer)-offset, hfp_indicators, hfp_indicators_nr);
    offset += snprintf(buffer+offset, sizeof(buffer)-offset, "\r\n");
    buffer[offset] = 0;
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_retrieve_supported_generic_status_indicators_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s=?\r\n", HFP_GENERIC_STATUS_INDICATOR); 
    // printf("retrieve_supported_generic_status_indicators %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_list_initital_supported_generic_status_indicators_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s?\r\n", HFP_GENERIC_STATUS_INDICATOR);
    // printf("list_initital_supported_generic_status_indicators %s\n", buffer);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_query_operator_name_format_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s=3,0\r\n", HFP_QUERY_OPERATOR_SELECTION);
    return send_str_over_rfcomm(cid, buffer);
}

int hfp_hs_query_operator_name_cmd(uint16_t cid){
    char buffer[20];
    sprintf(buffer, "AT%s?\r\n", HFP_QUERY_OPERATOR_SELECTION);
    return send_str_over_rfcomm(cid, buffer);
}

                
static void hfp_emit_ag_indicator_event(hfp_callback_t callback, hfp_ag_indicator_t indicator){
    if (!callback) return;
    uint8_t event[5];
    event[0] = HCI_EVENT_HFP_META;
    event[1] = sizeof(event) - 2;
    event[2] = HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED;
    event[3] = indicator.index; 
    event[4] = indicator.status; 
    (*callback)(event, sizeof(event));
}

static void hfp_run_for_context(hfp_connection_t * context){
    if (!context) return;
    // printf("hfp send cmd: context %p, RFCOMM cid %u \n", context, context->rfcomm_cid );
    if (!rfcomm_can_send_packet_now(context->rfcomm_cid)) return;
    
    switch (context->state){
        case HFP_EXCHANGE_SUPPORTED_FEATURES:
            hfp_hs_exchange_supported_features_cmd(context->rfcomm_cid);
            context->state = HFP_W4_EXCHANGE_SUPPORTED_FEATURES;
            break;
        case HFP_NOTIFY_ON_CODECS:
            hfp_hs_retrieve_codec_cmd(context->rfcomm_cid);
            context->state = HFP_W4_NOTIFY_ON_CODECS;
            break;
        case HFP_RETRIEVE_INDICATORS:
            hfp_hs_retrieve_indicators_cmd(context->rfcomm_cid);
            context->state = HFP_W4_RETRIEVE_INDICATORS;
            context->retrieve_ag_indicators = 1;
            context->retrieve_ag_indicators_status = 0;
            break;
        case HFP_RETRIEVE_INDICATORS_STATUS:
            hfp_hs_retrieve_indicators_status_cmd(context->rfcomm_cid);
            context->state = HFP_W4_RETRIEVE_INDICATORS_STATUS;
            context->retrieve_ag_indicators_status = 1;
            context->retrieve_ag_indicators = 0;
            break;
        case HFP_ENABLE_INDICATORS_STATUS_UPDATE:
            hfp_hs_activate_status_update_for_all_ag_indicators_cmd(context->rfcomm_cid, 1);
            context->state = HFP_W4_ENABLE_INDICATORS_STATUS_UPDATE;
            break;
        case HFP_RETRIEVE_CAN_HOLD_CALL:
            hfp_hs_retrieve_can_hold_call_cmd(context->rfcomm_cid);
            context->state = HFP_W4_RETRIEVE_CAN_HOLD_CALL;
            break;
        case HFP_LIST_GENERIC_STATUS_INDICATORS:
            hfp_hs_list_supported_generic_status_indicators_cmd(context->rfcomm_cid);
            context->state = HFP_W4_LIST_GENERIC_STATUS_INDICATORS;
            context->sent_command = HFP_CMD_LIST_GENERIC_STATUS_INDICATOR;
            context->list_generic_status_indicators = 1;
            break;
        case HFP_RETRIEVE_GENERIC_STATUS_INDICATORS:
            hfp_hs_retrieve_supported_generic_status_indicators_cmd(context->rfcomm_cid);
            context->state = HFP_W4_RETRIEVE_GENERIC_STATUS_INDICATORS;
            context->sent_command = HFP_CMD_GENERIC_STATUS_INDICATOR;
            context->retrieve_generic_status_indicators = 1;
            break;
        case HFP_RETRIEVE_INITITAL_STATE_GENERIC_STATUS_INDICATORS:
            hfp_hs_list_initital_supported_generic_status_indicators_cmd(context->rfcomm_cid);
            context->state = HFP_W4_RETRIEVE_INITITAL_STATE_GENERIC_STATUS_INDICATORS;
            context->sent_command = HFP_CMD_GENERIC_STATUS_INDICATOR_STATE;
            context->retrieve_generic_status_indicators_state = 1;

            break;
        case HFP_W2_DISCONNECT_RFCOMM:
            context->state = HFP_W4_RFCOMM_DISCONNECTED;
            rfcomm_disconnect_internal(context->rfcomm_cid);
            break;
        case HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED:{
            int i;
            for (i = 0; i < context->ag_indicators_nr; i++){
                if (context->ag_indicators[i].status_changed == 1) {
                    hfp_emit_ag_indicator_event(hfp_callback, context->ag_indicators[i]);
                    context->ag_indicators[i].status_changed = 0;
                    break;
                }
            }

            if (context->wait_ok == 1) return;

            if (context->enable_status_update_for_ag_indicators != 0xFF){
                hfp_hs_activate_status_update_for_all_ag_indicators_cmd(context->rfcomm_cid, context->enable_status_update_for_ag_indicators);
                context->wait_ok = 1;
                context->enable_status_update_for_ag_indicators = 0xFF;
                break;
            };
            if (context->change_status_update_for_individual_ag_indicators == 1){
                hfp_hs_activate_status_update_for_ag_indicator_cmd(context->rfcomm_cid, 
                        context->ag_indicators_status_update_bitmap,
                        context->ag_indicators_nr);
                context->wait_ok = 1;
                context->change_status_update_for_individual_ag_indicators = 0;
                break;
            }

            if (context->operator_name_format == 1){
                hfp_hs_query_operator_name_format_cmd(context->rfcomm_cid);
                context->operator_name_format = 0;
                context->operator_name = 1;
                context->wait_ok = 1;
                break;
            }
            if (context->operator_name == 1){
                hfp_hs_query_operator_name_cmd(context->rfcomm_cid);
                context->wait_ok = 1;
                context->operator_name = 0;
            }
            break;
        }
        default:
            break;
    }
}

void update_command(hfp_connection_t * context){
    context->command = HFP_CMD_NONE; 
    if (strncmp((char *)context->line_buffer, HFP_ERROR, strlen(HFP_ERROR)) == 0){
        context->command = HFP_CMD_ERROR;
        printf("Received ERROR\n");
    }

    if (strncmp((char *)context->line_buffer, HFP_OK, strlen(HFP_OK)) == 0){
        context->command = HFP_CMD_OK;
        printf("Received OK\n");
        return;
    }

    if (strncmp((char *)context->line_buffer, HFP_SUPPORTED_FEATURES, strlen(HFP_SUPPORTED_FEATURES)) == 0){
        printf("Received +BRSF\n");
        context->command = HFP_CMD_SUPPORTED_FEATURES;
        return;
    }

    if (strncmp((char *)context->line_buffer, HFP_INDICATOR, strlen(HFP_INDICATOR)) == 0){
        printf("Received +CIND, %d\n", context->sent_command);
        context->command = HFP_CMD_INDICATOR;
        return;
    }

    if (strncmp((char *)context->line_buffer, HFP_AVAILABLE_CODECS, strlen(HFP_AVAILABLE_CODECS)) == 0){
        printf("Received +BAC\n");
        context->command = HFP_CMD_AVAILABLE_CODECS;
        return;
    }

    if (strncmp((char *)context->line_buffer, HFP_ENABLE_STATUS_UPDATE_FOR_AG_INDICATORS, strlen(HFP_ENABLE_STATUS_UPDATE_FOR_AG_INDICATORS)) == 0){
        printf("Received +CMER\n");
        context->command = HFP_CMD_ENABLE_INDICATOR_STATUS_UPDATE;
        return;
    }

    if (strncmp((char *)context->line_buffer, HFP_SUPPORT_CALL_HOLD_AND_MULTIPARTY_SERVICES, strlen(HFP_SUPPORT_CALL_HOLD_AND_MULTIPARTY_SERVICES)) == 0){
        printf("Received +CHLD\n");
        context->command = HFP_CMD_SUPPORT_CALL_HOLD_AND_MULTIPARTY_SERVICES;
        return;
    } 

    if (strncmp((char *)context->line_buffer, HFP_GENERIC_STATUS_INDICATOR, strlen(HFP_GENERIC_STATUS_INDICATOR)) == 0){
        printf("Received +BIND\n");
        context->command = context->sent_command;
        printf("Received +BIND %d\n", context->sent_command);
        return;
    } 

    if (strncmp((char *)context->line_buffer, HFP_TRANSFER_AG_INDICATOR_STATUS, strlen(HFP_UPDATE_ENABLE_STATUS_FOR_INDIVIDUAL_AG_INDICATORS)) == 0){
        printf("Received +CIEV\n");
        context->command = HFP_CMD_TRANSFER_AG_INDICATOR_STATUS;
        return;
    } 
       
}


void handle_switch_on_ok(hfp_connection_t *context){
    // printf("switch on ok\n");
    switch (context->state){
        case HFP_W4_EXCHANGE_SUPPORTED_FEATURES:
            if (has_codec_negotiation_feature(context)){
                context->state = HFP_NOTIFY_ON_CODECS;
                break;
            } 
            context->state = HFP_RETRIEVE_INDICATORS;
            break;

        case HFP_W4_NOTIFY_ON_CODECS:
            context->state = HFP_RETRIEVE_INDICATORS;
            break;   
        
        case HFP_W4_RETRIEVE_INDICATORS:
            context->state = HFP_RETRIEVE_INDICATORS_STATUS; 
            context->retrieve_ag_indicators = 0;
            break;
        
        case HFP_W4_RETRIEVE_INDICATORS_STATUS:
            context->state = HFP_ENABLE_INDICATORS_STATUS_UPDATE;
            context->retrieve_ag_indicators_status = 0;
            break;
            
        case HFP_W4_ENABLE_INDICATORS_STATUS_UPDATE:
            if (has_call_waiting_and_3way_calling_feature(context)){
                context->state = HFP_RETRIEVE_CAN_HOLD_CALL;
                break;
            }
            if (has_hf_indicators_feature(context)){
                context->state = HFP_LIST_GENERIC_STATUS_INDICATORS;
                break;
            } 
            context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
            hfp_emit_event(hfp_callback, HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED, 0);
            break;
        
        case HFP_W4_RETRIEVE_CAN_HOLD_CALL:
            if (has_hf_indicators_feature(context)){
                context->state = HFP_LIST_GENERIC_STATUS_INDICATORS;
                break;
            } 
            context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
            hfp_emit_event(hfp_callback, HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED, 0);
           break;
        
        case HFP_W4_LIST_GENERIC_STATUS_INDICATORS:
            context->state = HFP_RETRIEVE_GENERIC_STATUS_INDICATORS;
            break;

        case HFP_W4_RETRIEVE_GENERIC_STATUS_INDICATORS:
            context->state = HFP_RETRIEVE_INITITAL_STATE_GENERIC_STATUS_INDICATORS;
            break;
                    
        case HFP_W4_RETRIEVE_INITITAL_STATE_GENERIC_STATUS_INDICATORS:
            printf("Supported initial state generic status indicators \n");
            context->state = HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED;
            hfp_emit_event(hfp_callback, HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED, 0);
            break;

        case HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED:
            context->wait_ok = 0;
            hfp_emit_event(hfp_callback, HFP_SUBEVENT_COMPLETE, 0);
           
            break;
        default:
            break;

    }
    // done
    context->command = HFP_CMD_NONE;
}

static void hfp_handle_rfcomm_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    hfp_connection_t * context = get_hfp_connection_context_for_rfcomm_cid(channel);
    if (!context) return;

    packet[size] = 0;
    int pos;
    printf("parse command: %s, state: %d\n", packet, context->parser_state);
    for (pos = 0; pos < size ; pos++){
        hfp_parse(context, packet[pos]);

        // trigger next action after CMD received
        if (context->command == HFP_CMD_ERROR){
            if (context->state == HFP_SERVICE_LEVEL_CONNECTION_ESTABLISHED){
                context->wait_ok = 0;
                // TODO: reset state? repeat commands? restore bitmaps? get ERROR codes.
                hfp_emit_event(hfp_callback, HFP_SUBEVENT_COMPLETE, 1); 
            } else {

            }
        }
        if (context->command != HFP_CMD_OK) continue;
        handle_switch_on_ok(context);
    }
}

static void hfp_run(){
    linked_list_iterator_t it;    
    linked_list_iterator_init(&it, hfp_get_connections());
    while (linked_list_iterator_has_next(&it)){
        hfp_connection_t * connection = (hfp_connection_t *)linked_list_iterator_next(&it);
        hfp_run_for_context(connection);
    }
}

static void packet_handler(void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    switch (packet_type){
        case RFCOMM_DATA_PACKET:
            hfp_handle_rfcomm_event(packet_type, channel, packet, size);
            break;
        case HCI_EVENT_PACKET:
            hfp_handle_hci_event(hfp_callback, packet_type, packet, size);
        default:
            break;
    }
    hfp_run();
}

void hfp_hf_init(uint16_t rfcomm_channel_nr, uint32_t supported_features, uint8_t * codecs, int codecs_nr, uint16_t * indicators, int indicators_nr, uint32_t indicators_status){
    if (codecs_nr > HFP_MAX_NUM_CODECS){
        log_error("hfp_init: codecs_nr (%d) > HFP_MAX_NUM_CODECS (%d)", codecs_nr, HFP_MAX_NUM_CODECS);
        return;
    }
    rfcomm_register_packet_handler(packet_handler);
    hfp_init(rfcomm_channel_nr);
    
    hfp_supported_features = supported_features;
    hfp_codecs_nr = codecs_nr;
    
    int i;
    for (i=0; i<codecs_nr; i++){
        hfp_codecs[i] = codecs[i];
    }

    hfp_indicators_nr = indicators_nr;
    hfp_indicators_status = indicators_status;
    for (i=0; i<indicators_nr; i++){
        hfp_indicators[i] = indicators[i];
    }
}

void hfp_hf_establish_service_level_connection(bd_addr_t bd_addr){
    hfp_establish_service_level_connection(bd_addr, SDP_HandsfreeAudioGateway);
}

void hfp_hf_release_service_level_connection(bd_addr_t bd_addr){
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    hfp_release_service_level_connection(connection);
    hfp_run_for_context(connection);
}

void hfp_hf_enable_status_update_for_all_ag_indicators(bd_addr_t bd_addr, uint8_t enable){
    hfp_hf_establish_service_level_connection(bd_addr);
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    if (!connection){
        log_error("HFP HF: connection doesn't exist.");
        return;
    }
    connection->enable_status_update_for_ag_indicators = 1;
    hfp_run_for_context(connection);
}

// TODO: returned ERROR - wrong format
void hfp_hf_enable_status_update_for_individual_ag_indicators(bd_addr_t bd_addr, uint32_t indicators_status_bitmap){
    hfp_hf_establish_service_level_connection(bd_addr);
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    if (!connection){
        log_error("HFP HF: connection doesn't exist.");
        return;
    }
    connection->change_status_update_for_individual_ag_indicators = 1;
    connection->ag_indicators_status_update_bitmap = indicators_status_bitmap; 
    hfp_run_for_context(connection);
}

void hfp_hf_query_operator_selection(bd_addr_t bd_addr){
    hfp_hf_establish_service_level_connection(bd_addr);
    hfp_connection_t * connection = get_hfp_connection_context_for_bd_addr(bd_addr);
    if (!connection){
        log_error("HFP HF: connection doesn't exist.");
        return;
    }
    connection->operator_name_format = 1;
    hfp_run_for_context(connection);
}
