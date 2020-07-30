/*********************************************************************
 Copyright 2020 CommScope, Inc. All rights reserved.

 This program is confidential and proprietary to CommScope, Inc.
 (CommScope), and may not be copied, reproduced, modified, disclosed
 to others, published or used, in whole or in part, without the
 express prior written permission of CommScope.
**********************************************************************/
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <wifidr_api_util.h>
#include <ctype.h>
#include "meshagent.h"
#include "ccsp/wifi_hal.h"
#include "wifi_doctor_agent_common.h"

/* MeshAgent Server message socket */
#define MESH_SOCKET_PATH_NAME   "\0/tmp/mesh-socket-queue"

#define DRIVER_INIT_TIMEOUT (20000)
#define RADIO_24G           (0)
#define RADIO_5G            (1)

typedef struct _Interface_Node_t
{
    bool is_allocated;
    WiFiDoctor_Interface_Update_t *interface_update;
}Interface_Node_t;

static Interface_Node_t interface_arr[16];

#define DHCP_OPTION_FILE_NAME "/tmp/dhcp_options.txt"
#define DHCP_FILE_NAME "/tmp/dnsmasq.lease"


static void* meshagent_sync_thread_entry(void *data);
//static void* WiFiDoctorSyncThread(void *data);
//static void processWiFiDoctorMessage(WiFiDoctorEventMsg rxMsg);
static void process_meshagent_event(MeshSync *p_mesh_sync_msg);
static void send_message_to_wifi_doctor_agent(WiFiDoctor_SyncMsg_t *p_wifidr_sync_msg);
static void send_notification_to_fhcd(eWiFiDoctorSyncType type);
static BOOL IsDriverInitialized(void);
static void get_host_info_from_mac(char *, WiFiDoctor_Client_Connect_Msg_t *);
static BOOL getHostFriendlyName(char *clientMac, char *result);
static int executeCmd(char *cmd, char *output, int len);
static int is_equal_macaddress(char* addr1, char* addr2);

#ifdef DEBUG
static char *eStr[] = {"MESH_WIFI_RESET", "MESH_WIFI_RADIO_CHANNEL", "MESH_WIFI_RADIO_CHANNEL_MODE",
                 "MESH_WIFI_SSID_NAME", "MESH_WIFI_SSID_ADVERTISE", "MESH_WIFI_AP_SECURITY",
                 "MESH_WIFI_AP_KICK_ASSOC_DEVICE", "MESH_WIFI_AP_KICK_ALL_ASSOC_DEVICES",
                 "MESH_WIFI_AP_ADD_ACL_DEVICE", "MESH_WIFI_AP_DEL_ACL_DEVICE","MESH_WIFI_MAC_ADDR_CONTROL_MODE",
                 "MESH_SUBNET_CHANGE","MESH_URL_CHANGE","MESH_WIFI_STATUS","MESH_WIFI_ENABLE","MESH_STATE_CHANGE",
                 "MESH_WIFI_TXRATE","MESH_CLIENT_CONNECT","MESH_DHCP_RESYNC_LEASES","MESH_DHCP_ADD_LEASE",
                 "MESH_DHCP_REMOVE_LEASE","MESH_DHCP_UPDATE_LEASE","MESH_WIFI_SUPER_WIFI_MODE","MESH_RADIO_ENABLE",
                 "MESH_VAP_ENABLE","MESH_WIFI_AP_ISOLATION","MESH_SYNC_MSG_TOTAL"
                };
#endif

int main(int argc, char** argv)
{
    pthread_t      meshEventThdId;
    void* val;
    int i;

    /* initialize interface update array */
    for(i = 0; i < 16; i++) {
        interface_arr[i].is_allocated = false;
    }

    /* Spawn mesh agent event listener thread.
     * This listens on events from mesh-agent */
    pthread_create(&meshEventThdId, NULL, meshagent_sync_thread_entry, NULL);

    /* Spawn WiFiDoctor agent event listener thread.
    * This listens on events from WiFi Doctor Agent */
    //pthread_create(&WiFiDoctorEventThdId, NULL, WiFiDoctorSyncThread, NULL);

    /* join threads to process */
    pthread_join(meshEventThdId, &val);


    /* free memory allocated for interface updates */
    for(i = 0; i < MAX_SSIDS; i++) {
        if(interface_arr[i].is_allocated) {
            free(interface_arr[i].interface_update);
        }
    }

    //pthread_join(WiFiDoctorEventThdId, &val);
    //WiFiDoctorSyncThread(NULL);
}

/*************************************************************************
* This thread receives mesh-agent notifications and sends them to
* WiFiDoctor agent.
*************************************************************************/
static void* meshagent_sync_thread_entry(void *data)
{
    int                client_sock = -1;
    int enable = 1;
    int                bytes_read = 0;
    char              *client_socket_path = "/tmp/naclient_sock";
    const char meshSocketPath[] = MESH_SOCKET_PATH_NAME;
    struct sockaddr_un server_sockaddr;
    struct sockaddr_un client_sockaddr;
    MeshSync           rxMsg = {0}; //received message
    //fd_set readfds;

    memset(&client_sockaddr, 0, sizeof(struct sockaddr_un));
    memset(&server_sockaddr, 0, sizeof(struct sockaddr_un));

    if( (client_sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
    {
        printf("Mesh Queue socket creation failure\n");
        return NULL;
    }

    /* fill client details to socket */
    client_sockaddr.sun_family = AF_UNIX;

    if (*client_socket_path == '\0') {
        *client_sockaddr.sun_path = '\0';
        strncpy(client_sockaddr.sun_path+1, client_socket_path+1, sizeof(client_sockaddr.sun_path)-2);
    } else {
        strncpy(client_sockaddr.sun_path, client_socket_path, sizeof(client_sockaddr.sun_path)-1);
        unlink(client_socket_path);
    }

    if(setsockopt(client_sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        printf("setsockopt(SO_REUSEADDR) failed\n");

    /* bind socket */
    if (bind(client_sock,
             (struct sockaddr *)&client_sockaddr,
             sizeof(client_sockaddr)) < 0)
    {
        printf("%s(): binding failed with reason[%d]\n", __func__, errno);
        shutdown(client_sock, SHUT_WR);
        close(client_sock);
        return NULL;
    }

    /* fill client details to socket */
    server_sockaddr.sun_family = AF_UNIX;

    /* server connection data*/
    if (*meshSocketPath == '\0') {
        *server_sockaddr.sun_path = '\0';
        strncpy(server_sockaddr.sun_path+1, meshSocketPath+1, sizeof(server_sockaddr.sun_path)-2);
    } else {
        strncpy(server_sockaddr.sun_path, meshSocketPath, sizeof(server_sockaddr.sun_path)-1);
    }

    /* in case mesh-agent's server socket is not ready, need to retry connect */
    while(1) {
        /* connect request to server */
        if (connect(client_sock, (struct sockaddr*)&server_sockaddr, sizeof(server_sockaddr)) == -1) {
            printf("%s(): connect failed with reason[%d]\n", __func__, errno);
            sleep(5); /* 5sec */
        }else {
           break;
        }
    }

    /* receive data from mesh-agent on the connected socket,
     * process and send it to wherever required
     * */
    while(1) {
        memset(&rxMsg, 0, sizeof(MeshSync));

        bytes_read = recv(client_sock, (void *)&rxMsg, sizeof(rxMsg), 0);

        if(bytes_read == 0) {
            printf("connection closed by peer[%d]\n", errno);
            break;
        }

        if(bytes_read > 0) {
            /* process message and take action */
            process_meshagent_event(&rxMsg);
        }
        else {
            printf("recv failed with reason[%d]\n", errno);
        }
    }

    shutdown(client_sock, SHUT_WR);

    close(client_sock);
    return NULL;
}

static int get_interface_details(int ap_index, Interface_Node_t *p_interface_node)
{
    WiFiDoctor_Interface_Update_t *interface = NULL;

    if(NULL == p_interface_node)
        return 1;

    /* allocate memory for interface details */
    p_interface_node->interface_update = interface =
            (WiFiDoctor_Interface_Update_t *)calloc(1, sizeof(WiFiDoctor_Interface_Update_t));

    if(NULL == interface)
        return 1;

    interface->id = ap_index+1;
    interface->radio_id = RADIO_INDEX(ap_index);

    wifi_getBaseBSSID(ap_index, interface->mac);

    wifi_getSSIDName(ap_index, interface->ssid);

    wifi_getApEnable(ap_index, &interface->state);

    interface->mode = 0; //WIFIDR_INTERFACE_MODE_AP

    /*
    * - 0 : filter as  disabled
    * - 1 : filter as whitelist
    * - 2 : filter as blacklist.
    */
    wifi_getApMacAddressControlMode(ap_index, &interface->acl_mode);
    if(2 == interface->acl_mode) {
        p_interface_node->interface_update->acl_mode = 0;//WIFIDR_MAC_ACL_MODE_DENY
    }
    else if (1 == interface->acl_mode){
        p_interface_node->interface_update->acl_mode = 1;//WIFIDR_MAC_ACL_MODE_ACCEPT
    }else {}

    wifi_getApIsolationEnable(ap_index, &interface->ap_isolation);

    wifi_getApSecurityModeEnabled(ap_index, interface->security_mode);

    p_interface_node->is_allocated = true;
    return 0;
}

/**********************************************************************************************
* This function processes the mesh-agent notification.
* Following usecases are covered:
* ============================================================================================
* SL. No       Event Type                   Airties API               Requests FHCd to
*                                                                       be notified
* ============================================================================================
* 1.       Radio Enable/Disable            wifidr_radio_update            Yes
* 2.       Channel Change                  wifidr_radio_update            No
* 3.       Wireless mode Change            wifidr_radio_update            No
*          (b/g/n/ac)
* 4.       ACS channel list change         wifidr_radio_update            No
* 5.       Interface Enable/Disable        wifidr_interface_update        Yes
* 6.       SSID change                     wifidr_interface_update        Yes
* 7.       AP Isolation Enable/Disable     wifidr_interface_update        No
* 8.       Passphrase change               wifidr_interface_update        Yes
* 9.       Security Mode                   wifidr_interface_update        Yes
* 10.      ACL Mode                        wifidr_interface_update        Yes
* 11.      STA Added/Removed from ACL      wifidr_interface_update        Yes
**********************************************************************************************/
static void process_meshagent_event(MeshSync *p_mesh_sync_msg)
{
    WiFiDoctor_SyncMsg_t wifidr_sync_msg = {0};

    if ((NULL == p_mesh_sync_msg) || (p_mesh_sync_msg->msgType >= MESH_SYNC_MSG_TOTAL))
    {
        printf("Error unknown message type - skipping\n");
        return;
    }

#ifdef DEBUG
    printf("%s(): message received from mesh-agent = %s\n", __func__, eStr[p_mesh_sync_msg->msgType]);
#endif

    switch (p_mesh_sync_msg->msgType)
    {
        case MESH_WIFI_RESET: /* aka MESH_RADIO_ENABLE */
        case MESH_WIFI_RADIO_CHANNEL:
        case MESH_WIFI_RADIO_CHANNEL_MODE:
        {
            /* WIFI_RESET is already been raised for Radio Enable/Disable.
             * The same is considered as MESH_RADIO_ENABLE */

            //void* radio_update_data = NULL;

            if(p_mesh_sync_msg->msgType == MESH_RADIO_ENABLE)
            {
                send_notification_to_fhcd(p_mesh_sync_msg->msgType);
            }

            if(IsDriverInitialized())
            {
                if(p_mesh_sync_msg->msgType == MESH_RADIO_ENABLE && p_mesh_sync_msg->data.apState.enable == false)
                {
#if 0
                    radio_stats.radio_state = false;
                    memcpy((char *)&radio_stats,
                           (char *)&g_radio_stats,
                           sizeof(wifi_cs_radio_update_stats_t));
#endif
                }

                /* HAL API to fetch radio details, currently data stubbed in agent */
#if 0
                if(0 != (ret = wifi_cs_getRadioUpdate(p_mesh_sync_msg->data.wifiRadioChannel.index,
                                                      radio_update_data))) {
                    //wifidr_log_e(WIFIDR_LOG_PI, "wifi_cs_getRadioUpdate failed [ret=%d]", ret);
                    printf("wifi_cs_getRadioUpdate failed [ret=%d]", ret);
                    return;
                }
#endif
                /* format message to send to Doctor Agent */
                wifidr_sync_msg.msgType = COMM_RADIO_UPDATE;
                /*memcpy((char *)&wifidr_sync_msg.data.radio_update,
                       (char *)&radio_update_data,
                        sizeof(wifi_cs_radio_t));*/

                send_message_to_wifi_doctor_agent(&wifidr_sync_msg);
            }

            break;
        }

        case WIFIDOCTOR_ACS_LIST_CHANGE:
        {
            /* to be posted from doctor agent in configure_best_channels callback */
            //radio update
            break;
        }

        case MESH_VAP_ENABLE:
        case MESH_WIFI_SSID_NAME:
        case MESH_WIFI_AP_SECURITY:
        case MESH_WIFI_MAC_ADDR_CONTROL_MODE:
        case MESH_WIFI_AP_ADD_ACL_DEVICE:
        case MESH_WIFI_AP_DEL_ACL_DEVICE:
        case MESH_WIFI_AP_ISOLATION:
        {
            //wifi_cs_interface_update_stats_t ap_stats;
            Interface_Node_t *p_interface_node = NULL;

            if(p_mesh_sync_msg->msgType != MESH_WIFI_AP_ISOLATION) {
                send_notification_to_fhcd(p_mesh_sync_msg->msgType);
            }

            if(IsDriverInitialized()) {
                if(p_mesh_sync_msg->msgType == MESH_VAP_ENABLE &&
                              p_mesh_sync_msg->data.apState.enable == false)
                {
#if 0
                    ap_stats.state = false;
                    memcpy((char *)&ap_stats,
                           (char*)&g_ap_stats,
                            sizeof(wifi_cs_interface_update_stats_t));
#endif
                }
                else
                {
                    /* format message to send to Doctor Agent */
                    wifidr_sync_msg.msgType = COMM_INTERFACE_UPDATE;

                    if(MESH_VAP_ENABLE == p_mesh_sync_msg->msgType) {
                        MeshApStateChange *apState = &p_mesh_sync_msg->data.apState;

                        p_interface_node = &interface_arr[apState->index];
                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(apState->index, p_interface_node)) {
                                return;
                            }
                        }
                        p_interface_node->interface_update->state = apState->enable;
                    }

                    if(MESH_WIFI_AP_SECURITY == p_mesh_sync_msg->msgType) {
                        MeshWifiAPSecurity *wifiAPSecurity = &p_mesh_sync_msg->data.wifiAPSecurity;

                        p_interface_node = &interface_arr[wifiAPSecurity->index];
                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(wifiAPSecurity->index, p_interface_node)) {
                                return;
                            }
                        }
                        strcpy(p_interface_node->interface_update->security_mode, wifiAPSecurity->secMode);
                    }

                    if(MESH_WIFI_SSID_NAME == p_mesh_sync_msg->msgType) {
                        MeshWifiSSIDName *wifiSSIDName = &p_mesh_sync_msg->data.wifiSSIDName;

                        p_interface_node = &interface_arr[wifiSSIDName->index];

                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(wifiSSIDName->index, p_interface_node)) {
                                return;
                            }
                        }
                        strncpy(p_interface_node->interface_update->ssid, (const char *)wifiSSIDName->ssid, sizeof(p_interface_node->interface_update->ssid)-1);
                    }

                    if(MESH_WIFI_MAC_ADDR_CONTROL_MODE == p_mesh_sync_msg->msgType) {
                        MeshWifiMacAddrControlMode *wifiMacAddrControlMode = &p_mesh_sync_msg->data.wifiMacAddrControlMode;

                        p_interface_node = &interface_arr[wifiMacAddrControlMode->index];
                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(wifiMacAddrControlMode->index, p_interface_node)) {
                                return;
                            }
                        }

                        if(wifiMacAddrControlMode->isEnabled) {
                            if(wifiMacAddrControlMode->isBlacklist) {
                                p_interface_node->interface_update->acl_mode = 0;//WIFIDR_MAC_ACL_MODE_DENY
                            }
                            else {
                                p_interface_node->interface_update->acl_mode = 1;//WIFIDR_MAC_ACL_MODE_ACCEPT
                            }
                        }
                    }

                    if(MESH_WIFI_AP_ADD_ACL_DEVICE == p_mesh_sync_msg->msgType) {
                        MeshWifiAPAddAclDevice *wifiAPAddAclDevice = &p_mesh_sync_msg->data.wifiAPAddAclDevice;

                        p_interface_node = &interface_arr[wifiAPAddAclDevice->index];
                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(wifiAPAddAclDevice->index, p_interface_node)) {
                                return;
                            }
                            /* add code to retrieve acl list */
                        }
                    }

                    if(MESH_WIFI_AP_DEL_ACL_DEVICE == p_mesh_sync_msg->msgType) {
                        MeshWifiAPDelAclDevice *wifiAPDelAclDevice = &p_mesh_sync_msg->data.wifiAPDelAclDevice;

                        p_interface_node = &interface_arr[wifiAPDelAclDevice->index];
                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(wifiAPDelAclDevice->index, p_interface_node)) {
                                return;
                            }
                            /* TODO add code to retrieve acl list */
                        }
                    }

                    if(MESH_WIFI_AP_ISOLATION == p_mesh_sync_msg->msgType) {
                        MeshApIsolationChange *apIsolation = &p_mesh_sync_msg->data.apIsolation;

                        p_interface_node = &interface_arr[apIsolation->index];
                        if(!p_interface_node->is_allocated) {
                            /* populate interface details */
                            if(0 != get_interface_details(apIsolation->index, p_interface_node)) {
                                return;
                            }
                        }
                        p_interface_node->interface_update->ap_isolation = apIsolation->enable;
                    }

                    memcpy(&wifidr_sync_msg.data.interface_update,
                           p_interface_node->interface_update,
                           sizeof(wifidr_sync_msg.data.interface_update));

                    send_message_to_wifi_doctor_agent(&wifidr_sync_msg);
                }
            }
            break;
        }

        case MESH_CLIENT_CONNECT:
        {
            MeshClientConnect *meshConnect   = &p_mesh_sync_msg->data.meshConnect;
            WiFiDoctor_Client_Connect_Msg_t  *clientconnect = &wifidr_sync_msg.data.clientconnect;

            wifidr_sync_msg.msgType = COMM_HOST_UPDATE;

            if(meshConnect->iface == MESH_IFACE_ETHERNET)
            {
                clientconnect->ifaceType = COMM_TYPE_ETHERNET;
            }

            if(meshConnect->iface == MESH_IFACE_WIFI)
            {
                clientconnect->ifaceType = COMM_TYPE_WIFI;
            }

            memcpy(clientconnect->hostName,
                      meshConnect->host,
                      sizeof(clientconnect->hostName));
            memcpy(clientconnect->macAddress,
                      meshConnect->mac,
                      sizeof(clientconnect->macAddress));

            clientconnect->connection_state = (meshConnect->isConnected == 1) ? COMM_STATE_CONNECTED: COMM_STATE_DISCONNECTED;
            get_host_info_from_mac(meshConnect->mac,clientconnect);

            send_message_to_wifi_doctor_agent(&wifidr_sync_msg);
            break;
        }

        default:
            break;
    }
}

/*************************************************************************
* This function sends the collected WiFi Statistics to
* WiFi Doctor notification.
*************************************************************************/
static void send_message_to_wifi_doctor_agent(WiFiDoctor_SyncMsg_t *p_wifidr_sync_msg)
{
    char               *socket_path = NOTIFICATION_AGENT_SOCKET_PATH_NAME;
    int                client_sock = -1;
    struct sockaddr_un address;
    int                bytes_sent = 0;

    if( (client_sock = socket(AF_UNIX, SOCK_DGRAM, 0)) == -1)
    {
        printf("Mesh Queue socket creation failure\n");
        return;
    }

    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path);

    bytes_sent = sendto(client_sock,
                        (char *)p_wifidr_sync_msg,
                         sizeof(WiFiDoctor_SyncMsg_t),
                         0,
                         (struct sockaddr *) &address,
                         sizeof(address));

    if (bytes_sent == -1) {
        printf("Failed to send Sync message to WiFi Doctor agent.\n");
    }

    close(client_sock);

    return;
}

#if 0
/*************************************************************************
* This thread receives WiFi Doctor notifications.
*************************************************************************/
static void* WiFiDoctorSyncThread(void *data)
{
    char              *socket_path       = "/tmp/WiFiDoctor_socket_event_path";
    int                connection_socket = -1;
    int                action            = -1;
    struct sockaddr_un local_address, peer_address;
    WiFiDoctorEventMsg rxMsg             = {0};
    fd_set             readfds;

    if( (connection_socket = socket(AF_UNIX, SOCK_DGRAM, 0)) == -1)
    {
        printf("Mesh Queue socket creation failure(errno=%d)\n",errno);
        return NULL;
    }

    local_address.sun_family = AF_UNIX;

    if (*socket_path == '\0') {
        *local_address.sun_path = '\0';
        strncpy(local_address.sun_path+1,
                socket_path+1,
                sizeof(local_address.sun_path)-2);
    } else {
        strncpy(local_address.sun_path,
                socket_path,
                  sizeof(local_address.sun_path)-1);

        unlink(socket_path);
    }

    if (bind(connection_socket,
             (struct sockaddr *)&local_address,
            sizeof(local_address))<0)
    {
        printf("Mesh Queue socket bind failure (errno=%d)\n",errno);
        return NULL;
    }

    while(1)
    {
        FD_ZERO(&readfds);

        FD_SET(connection_socket, &readfds);

        action = select(connection_socket + 1,
                        &readfds,
                        NULL,
                        NULL,
                        NULL);
        if ((action < 0) && (errno!=EINTR))
        {
            continue;
        }

        if (FD_ISSET(connection_socket, &readfds))
        {
            socklen_t address_len = sizeof(peer_address);
            if (recvfrom(connection_socket,
                        (void *)&rxMsg,
                        sizeof(WiFiDoctorEventMsg),
                        0,
                        (struct sockaddr *) &peer_address,
                        &address_len) != 0)
            {
                processWiFiDoctorMessage(rxMsg);
            }
        }
    }

    close(connection_socket);
    return NULL;
}

/*************************************************************************
* This function processes the WiFi Doctor notification.
*************************************************************************/
static void processWiFiDoctorMessage(WiFiDoctorEventMsg rxMsg)
{
    switch (rxMsg.eventType) {
        case WIFIDOCTOR_ACS_LIST_CHANGE:
        {
            WiFiDoctorSync syncMsg;
            wifi_cs_radio_update_stats_t radio_stats;

            if(IsDriverInitialized())
            {
                //wifi_cs_getRadioUpdate(rxMsg.data.acsChannelListUpdate.radioIndex,&radio_stats);
                memcpy((char *)&g_radio_stats,
                (char *)&radio_stats,
                sizeof(wifi_cs_radio_update_stats_t));

                syncMsg.msgType = COMM_RADIO_UPDATE;
                memcpy((char *)&syncMsg.data.radio_update,
                (char *)&radio_stats,
                sizeof(wifi_cs_radio_update_stats_t));
                send_message_to_wifi_doctor_agent(&syncMsg);
           }
        }
        break;

        default:
            break;
    }
}
#endif

static void send_notification_to_fhcd(eWiFiDoctorSyncType type)
{
    //TODO
}

/****************************************************************************
* This function checks if Driver is initialized else it waits for 10 seconds
* to check the driver initialization.
****************************************************************************/

static BOOL IsDriverInitialized(void)
{
    BOOL           driver_state  = FALSE;
#ifdef THREAD_BASED
    pthread_attr_t attr;
    pthread_t      tid;
    int            thread_status;

    thread_status = pthread_attr_init()(&tattr);
    thread_status = pthread_attr_setdetachstate()(&attr,PTHREAD_CREATE_DETACHED);
    thread_status = pthread_create()(&tid, &attr, driverStateThread, (void*)&event);
#else
    BOOL           radio_status  = FALSE;
    BOOL           radio_enabled = FALSE;
    int            timeout       = DRIVER_INIT_TIMEOUT; //millisecond.
    int            radioIndex    = RADIO_5G; //Default to 5G Radio

    if( wifi_getRadioEnable(1,&radio_enabled) == RETURN_OK && radio_enabled == FALSE)
    {
        if( wifi_getRadioEnable(0,&radio_enabled) == RETURN_OK && radio_enabled == FALSE)
        {
            radioIndex = 0;
        }
        else
        {
            return FALSE;
        }
    }

    while(driver_state == FALSE || timeout > 0 )
    {
        if( wifi_getRadioStatus(radioIndex,&radio_status) == RETURN_OK && radio_status == TRUE)
        {
            driver_state = TRUE;
            break;
        }
        else
        {
            sleep(1);
            timeout -= 1000;
        }
    }

    return driver_state;
#endif
}

/*
 * This code is to monitor whether driver completely restarted
 * */

#ifdef THREAD_BASED
void *driverStateThread(void *arg)
{
    bool           radio_status  = false;
    bool           radio_enabled = false;
    int            timeout       = 10000; //millisecond.
    int            radioIndex    = 1; //Default to 5G Radio
}
#endif

/*
  Brief: To obtain wifidr_host_t data
  Input: mac-macaddress and pointer to WiFiDoctor_Client_Connect_Msg_t
  Output: Updates WiFiDoctor_Client_Connect_Msg_t data members
*/
static void get_host_info_from_mac(char *clientMac, WiFiDoctor_Client_Connect_Msg_t *clientconnect)
{
    #define LEASE_MAC_POS (1)
    #define LEASE_IP_POS (2)
    #define LEASE_HOST_NAME (3)

    /* File pointer for dhcp_option and dnsmsq.lease */
    FILE *fp_dhcp_options = NULL;
    char lineBuffer[BUFF_LEN_2048] = {0};
    char *token = NULL;
    char *restOfString = NULL;
    char *elements = NULL;
    int endLineParsing = 0, count = 0;
    char dhcpClientOptions[BUFF_LEN_1024] = {0};
    char clientMacAddress[MAX_MAC_STR_LEN] = {0};
    char *marker = NULL;
    char *position = NULL;

    if((NULL == clientMac) || (NULL == clientconnect))
    {
        return;
    }
    /*
       dhcp_options file contains data format as
       <mac address>@<comma seperated dhcp option-55>&<Vendor class name>
       example:
        d0:37:45:5a:34:74@1,3,6,15,31,33,43,44,46,47,119,121,249,252&MSFT
    */
    fp_dhcp_options  = fopen(DHCP_OPTION_FILE_NAME,"r");
    if(NULL != fp_dhcp_options)
    {
        while( (fgets(lineBuffer, BUFF_LEN_2048, fp_dhcp_options)) != NULL )
        {
            /* clientMacAddress till delimiter @ */
            marker = lineBuffer;
            if((position = strchr(marker,'@')) != NULL)
            {
                memcpy((char *)clientMacAddress, marker, position-marker);
                marker = position + 1;

                if(is_equal_macaddress(clientMacAddress, clientMac))
                {
                    /* Read client dhcp option-55 till delimiter &*/
                    if((position = strchr(marker,'&')) != NULL)
                    {
                        memcpy((char *)dhcpClientOptions, marker, position-marker);
                        marker = position + 1;
                        token = dhcpClientOptions;
                        while((elements = strtok_r(token,",",&token)) && (clientconnect->nb_params < BUFF_LEN_256-1))
                        {
                            clientconnect->param_req_list[clientconnect->nb_params++]= (uint8_t) atoi(elements);
                        }

                        /* To obtain vendor class name */
                        if(marker != NULL)
                        {
                            memcpy(clientconnect->vendor_class, marker, strlen(marker));
                        }
                    }
                    break;
                }
            }
        }
        fclose(fp_dhcp_options);
    }

    /*
       Breif:
        File dnsmsq.lease contains data
        Fields in order
        1.Time of lease expiry, in epoch time
        2.MAC address
        3.IP address
        4.Computer Name
        5.Client-ID
        Example:
           1492815392 04:a1:51:cf:be:73 192.168.0.69 LP-MABROOKS-01 01:04:a1:51:cf:be:73
       Operation:
          * To read file till desired "clientMac" is endLineParsing
    */
    fp_dhcp_options = fopen(DHCP_FILE_NAME,"r");
    if(NULL != fp_dhcp_options)
    {
        while( (fgets(lineBuffer, BUFF_LEN_2048, fp_dhcp_options)) != NULL && !endLineParsing)
        {
            restOfString = lineBuffer;
            count = 0;
            /* In each line,match for clientMac address*/
            for(token = strtok_r(lineBuffer," ",&restOfString);
                        token != NULL;
                        token = strtok_r(NULL," ",&restOfString))
            {
                if((count == LEASE_MAC_POS) && (is_equal_macaddress(clientMac,token) != 1))
                {
                    break;
                }
                if(count == LEASE_IP_POS)
                {
                    snprintf(clientconnect->ip_address,sizeof(clientconnect->ip_address),"%s",token);
                    endLineParsing = 1;
                    break;
                }
                count++;
            }
        }

        fclose(fp_dhcp_options);
    }

    /*friendly name value*/
    getHostFriendlyName(clientMac,clientconnect->user_friendly_name);

}


/*
    Brief:
        To obtain Client "User friendly name"
    Input:
        Client macaddress and WiFiDoctor_Client_Connect_Msg_t userfriendly pointer
    Operation:
        * Determine if clientMac address is present , if so
        * Execute TR181 Device.Hosts.Host.{instance}.X_ARRIS_COM_FriendlyName
*/
static BOOL getHostFriendlyName(char *clientMac, char *result)
{
    BOOL status = FALSE;
    char cmd[BUFF_LEN_256]={0};
    char output[BUFF_LEN_512]={0};
    int instance = 1;
    char *subString = NULL;
    char queryFormat[10] = {0};
    char queryResult[BUFF_LEN_512] = {0};

    if(NULL == clientMac)
    {
        return status;
    }

    do
    {
        snprintf(cmd, sizeof(cmd)-1, "dmcli eRT getv Device.Hosts.Host.%d.PhysAddress", instance);
        executeCmd(cmd, output, sizeof(output));
        if(strstr(output, "Execution fail"))
        {
            break;
        }
        else if(strstr(output, "Execution succeed"))
        {
            if(strcasestr(output, clientMac))
            {
                /* Parse the output to get the friendly name value*/
                memset(cmd, 0, sizeof(cmd));
                memset(output, 0, sizeof(output));
                snprintf(cmd, sizeof(cmd)-1, "dmcli eRT getv Device.Hosts.Host.%d.X_ARRIS_COM_FriendlyName", instance);
                executeCmd(cmd, output, sizeof(output));
                if(strstr(output, "Execution succeed"))
                {
                    /* Format specifier with desired length,to avoid overflow */
                    snprintf(queryFormat,sizeof(queryFormat)," %%%dc",BUFF_LEN_512-1);
                    if((subString = strstr(output,"value:")) != NULL)
                    {
                        if(1 == sscanf(subString+strlen("value:"),queryFormat,queryResult))
                        {
                            memmove(result,queryResult,strlen(queryResult));
                            status = TRUE;
                        }
                    }
                }
                break;
            }
            else
            {
                ++instance;
            }
        }
        else
        {
            break;
        }

        memset(cmd, 0, sizeof(cmd));
        memset(output, 0, sizeof(output));
    }while(1);

    return status;
}

static int executeCmd(char *cmd, char *output, int len)
{
    int status = -1;
    FILE *fp = NULL;
    char   buf[BUFF_LEN_512] = {0};

    fp = popen(cmd, "r");
    if(fp)
    {
        while(fgets(buf, sizeof(buf), fp))
        {
            if((len -= strlen(buf)) <=0)
            {
                break;
            }
            strcat(output, buf);
            memset(buf, 0 , sizeof(buf));
        }
        pclose(fp);
        status = 0;
    }

    return status;
}

static int is_equal_macaddress(char* addr1, char* addr2)
{
    int i;
    for (i = 0; addr1[i]!='\0' ||  addr2[i]!= '\0'; i++)
    {
        if(addr1[i] >= 'a' && addr1[i] <= 'z')
        {
            addr1[i] = addr1[i] -32;
        }
        if(addr2[i] >= 'a' && addr2[i] <= 'z')
        {
            addr2[i] = addr2[i] -32;
        }
    }
    return (strcmp(addr1,addr2) == 0)?1:0;
}
