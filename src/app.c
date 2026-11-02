#include "app.h"
#include "setup_secret.h"
#include <stdio.h>
#include <string.h>

Config config;
bool configured,ap_mode;
char ap_name[32];
static uint32_t reboot_at;
static bool have_time;
static int ntp_fd=-1;
static uint32_t ntp_started,ntp_next;
static uint8_t ntp_cookie[8];

bool app_save_config(Config *c) {
    config_seal(c);return config_valid(c)&&fs_save(CONFIG_PATH,c,sizeof *c);
}
bool app_set_volume(unsigned v) {
    if(v>100||app_busy()||audio_playing())return false;
    if(config.volume==v)return true;
    Config updated=config;updated.volume=v;
    if(configured&&!app_save_config(&updated))return false;
    config.volume=v;return true;
}
bool app_busy(void) { return ota_busy()||sound_busy()||reboot_at!=0; }
void app_reboot_later(void) { reboot_at=millis()+800;if(!reboot_at)reboot_at=1; }
bool app_time_valid(void) { return have_time; }
void platform_idle_hook(void) { audio_poll(); }
void app_set_time(uint32_t seconds) {
    if(seconds<1704067200u||seconds>2085978495u)return;
    uint32_t days=seconds/86400,rem=seconds%86400;
    unsigned year=1970,month=1;
    for(;;year++) { unsigned count=365+(year%4==0&&(year%100!=0||year%400==0));if(days<count)break;days-=count; }
    static const uint8_t mdays[]={31,28,31,30,31,30,31,31,30,31,30,31};
    for(;month<=12;month++) { unsigned count=mdays[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));if(days<count)break;days-=count; }
    SlDateTime_t t={0};t.sl_tm_year=year;t.sl_tm_mon=month;t.sl_tm_day=days+1;
    t.sl_tm_hour=rem/3600;t.sl_tm_min=(rem%3600)/60;t.sl_tm_sec=rem%60;
    have_time=sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION,SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME,sizeof t,(_u8*)&t)>=0;
}
static void time_poll(void) {
    if(ap_mode||!configured||!config.mqtt_tls||!wifi_ip_ready||audio_playing())return;
    uint32_t now=millis();
    if(ntp_fd>=0) {
        uint8_t packet[48];int n=sl_Recv(ntp_fd,packet,sizeof packet,0);
        if(n==48 && (packet[0]&7)==4 && (packet[0]>>6)!=3 && packet[1]>0 && packet[1]<16 &&
           !memcmp(packet+24,ntp_cookie,8)) {
            uint32_t t=(uint32_t)packet[40]<<24|(uint32_t)packet[41]<<16|(uint32_t)packet[42]<<8|packet[43];
            if(t>=2208988800u)app_set_time(t-2208988800u);
            sl_Close(ntp_fd);ntp_fd=-1;ntp_next=now+(have_time?3600000:60000);
        } else if((n<0&&n!=SL_EAGAIN)||(uint32_t)(now-ntp_started)>3000) {
            sl_Close(ntp_fd);ntp_fd=-1;ntp_next=now+60000;
        }
    } else if((int32_t)(now-ntp_next)>=0) {
        uint32_t ip;if(!ip_parse(config.ntp_server,&ip)){ntp_next=now+60000;return;}
        ntp_fd=sl_Socket(SL_AF_INET,SL_SOCK_DGRAM,SL_IPPROTO_UDP);
        if(ntp_fd<0){ntp_next=now+60000;return;}
        SlSockAddrIn_t a={0};a.sin_family=SL_AF_INET;a.sin_port=sl_Htons(123);a.sin_addr.s_addr=sl_Htonl(ip);
        if(sl_Connect(ntp_fd,(SlSockAddr_t*)&a,sizeof a)<0||socket_nonblocking(ntp_fd)<0) {
            sl_Close(ntp_fd);ntp_fd=-1;ntp_next=now+60000;return;
        }
        uint8_t packet[48]={0};packet[0]=0x23;
        memcpy(ntp_cookie,device_mac,6);ntp_cookie[6]=now>>8;ntp_cookie[7]=now;
        memcpy(packet+40,ntp_cookie,8);
        if(sl_Send(ntp_fd,packet,sizeof packet,0)!=sizeof packet){sl_Close(ntp_fd);ntp_fd=-1;ntp_next=now+60000;}
        ntp_started=now;
    }
}
static bool start_ap(void) {
    ap_mode=true;wifi_ip_ready=false;
    snprintf(ap_name,sizeof ap_name,"ChimeSetup-%02X%02X%02X",device_mac[3],device_mac[4],device_mac[5]);
    uint8_t sec=SL_SEC_TYPE_WPA_WPA2,channel=6,hidden=0;
    if(sl_WlanSetMode(ROLE_AP)<0||
       sl_WlanSet(SL_WLAN_CFG_AP_ID,WLAN_AP_OPT_SSID,strlen(ap_name),(_u8*)ap_name)<0||
       sl_WlanSet(SL_WLAN_CFG_AP_ID,WLAN_AP_OPT_HIDDEN_SSID,1,&hidden)<0||
       sl_WlanSet(SL_WLAN_CFG_AP_ID,WLAN_AP_OPT_SECURITY_TYPE,1,&sec)<0||
       sl_WlanSet(SL_WLAN_CFG_AP_ID,WLAN_AP_OPT_PASSWORD,strlen(SETUP_PASSWORD),(_u8*)SETUP_PASSWORD)<0||
       sl_WlanSet(SL_WLAN_CFG_AP_ID,WLAN_AP_OPT_CHANNEL,1,&channel)<0)return false;
    SlNetCfgIpV4Args_t ip={0};ip.ipV4=0xc0a80401;ip.ipV4Mask=0xffffff00;
    ip.ipV4Gateway=ip.ipV4;ip.ipV4DnsServer=ip.ipV4;
    if(sl_NetCfgSet(SL_IPV4_AP_P2P_GO_STATIC_ENABLE,1,sizeof ip,(_u8*)&ip)<0)return false;
    sl_Stop(200);if(sl_Start(NULL,NULL,NULL)!=ROLE_AP)return false;
    SlNetAppDhcpServerBasicOpt_t dhcp={0};dhcp.lease_time=3600;
    dhcp.ipv4_addr_start=0xc0a80402;dhcp.ipv4_addr_last=0xc0a80414;
    sl_NetAppStop(SL_NET_APP_DHCP_SERVER_ID);
    if(sl_NetAppSet(SL_NET_APP_DHCP_SERVER_ID,NETAPP_SET_DHCP_SRV_BASIC_OPT,sizeof dhcp,(_u8*)&dhcp)<0||
       sl_NetAppStart(SL_NET_APP_DHCP_SERVER_ID)<0)return false;
    wifi_ip=0xc0a80401;wifi_ip_ready=true;
    log_line(ap_name);log_line("Configurator: http://192.168.4.1");return true;
}
static bool start_network(void) {
    if(!configured)return start_ap();
    if(sl_WlanSetMode(ROLE_STA)<0)return false;
    sl_WlanPolicySet(SL_POLICY_CONNECTION,SL_CONNECTION_POLICY(0,0,0,0,0),NULL,0);
    uint8_t enabled=1;sl_NetCfgSet(SL_IPV4_STA_P2P_CL_DHCP_ENABLE,1,1,&enabled);
    sl_Stop(200);if(sl_Start(NULL,NULL,NULL)!=ROLE_STA)return false;
    SlSecParams_t sec={0};sec.Type=config.wifi_password[0]?SL_SEC_TYPE_WPA_WPA2:SL_SEC_TYPE_OPEN;
    sec.Key=(_i8*)config.wifi_password;sec.KeyLen=strlen(config.wifi_password);
    return sl_WlanConnect((_i8*)config.ssid,strlen(config.ssid),NULL,&sec,NULL)>=0;
}
int main(void) {
    board_init();board_start_indicator();log_line("Chime NG " FW_VERSION " app");
    if(sl_Start(NULL,NULL,NULL)<0)for(;;){}
    sl_WlanPolicySet(SL_POLICY_CONNECTION,SL_CONNECTION_POLICY(0,0,0,0,0),NULL,0);sl_WlanDisconnect();
    sl_WlanPolicySet(SL_POLICY_SCAN,0,NULL,0);
    uint8_t len=6;
    if(sl_NetCfgGet(SL_MAC_ADDRESS_GET,NULL,&len,device_mac)<0||len!=6){log_line("MAC read failed");for(;;){}}
    int fd=fs_open_read(CONFIG_PATH);
    if(fd>=0){configured=fs_read(fd,0,&config,sizeof config)==sizeof config&&config_valid(&config);fs_close(fd);}
    if(!configured){memset(&config,0,sizeof config);config.volume=25;config.mqtt_port=1883;}
    if(!start_network()&&!start_ap())for(;;){}
    /* http_start disables TI's port-80 service before binding our listener. */
    sl_NetAppStop(SL_NET_APP_MDNS_ID);
    sound_init();
    if(audio_init())log_line("Codec and installed WAV initialized");else log_line(audio_error());
    BootRecord boot;
    /* The initial/previously committed image remains updatable even if its audio
     * bring-up needs a correction. Only a pending OTA image is a boot trial. */
    if(boot_read(&boot)&&boot.pending==NO_SLOT)ota_confirm();
    uint32_t started=millis(),stable=0,offline=0,web_retry=0;bool web=false;
    for(;;) {
        watchdog_feed();network_pump();audio_poll();time_poll();
        if(!ap_mode&&!wifi_ip_ready) {
            if(web){http_stop();web=false;web_retry=millis();}
            if(!offline)offline=millis();
            if((uint32_t)(millis()-offline)>30000) {
                http_stop();mqtt_shutdown();if(!start_ap())for(;;){}
                sl_NetAppStop(SL_NET_APP_MDNS_ID);web=false;web_retry=millis();
            }
        } else offline=0;
        if(wifi_ip_ready&&!web&&(int32_t)(millis()-web_retry)>=0) {
            web=http_start();if(web)stable=millis();else web_retry=millis()+5000;
        }
        if(web){http_poll();if(!http_running()){web=false;web_retry=millis()+5000;}}
        /* An unavailable broker must never prevent access to recovery/configuration.
         * Keep the update/configuration path available when audio needs repair.
         * Audio faults remain visible and inhibit playback, not OTA acceptance. */
        if(web&&!ota_confirmed()&&(uint32_t)(millis()-stable)>5000) {
            if(ota_confirm())log_line("Application confirmed");
        }
        if(!ota_confirmed()&&(uint32_t)(millis()-started)>90000)reboot();
        mqtt_poll();
        if(reboot_at&&(int32_t)(millis()-reboot_at)>=0){mqtt_shutdown();reboot();}
    }
}
