#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#include <syslog.h>
#include <math.h>
#include <mosquitto.h>

#define CLIENT_ID "bbox"               // An ID to use

// Default values if not specified in command line
#define TOPIC  "bbox/loop"             // What to publish as
#define BROKER_USERNAME "bbox"
#define BROKER_PASSWD   "bluetit"
#define BROKER_IP       "weather.ednet.pri"
#define BROKER_PORT     1883
#define LOOP_INTERVAL   5            // How often to publish

#define MAX_DS18B20     6             // Maximum number of DS18B20 sensors

#define WATCHDOG_TIMEOUT 20          // How many calls to mosquitto_loop
                                     // before going back to INIT state
#define WATCHDOG_INVALID -1

#define MOS_BLOCKTIME   1000         // How many ms for mosquitto_loop to block
                                     // on its select() call

#define INVALID_TEMP    999.0        // 

#define MAX_TEMP_CHANGE 5.0          // Maximum allowed change between
                                     // consecutive readings

#define W1_DIR          "/sys/bus/w1/devices"

#define MAX_TIMERS      2            // Total number of timers
#define TIMER_PUB       0            // ID for publishing scheduler
#define TIMER_WDOG      1            // ID for watchdog timer

typedef enum
{
    INIT,
    DISCONNECTED,
    DISCONNECTING,
    CONNECTING,
    PUBLISHING
} PUB_STATE;

char *state_names[] =
{
    "INIT",
    "DISCONNECTED",
    "DISCONNECTING",
    "CONNECTING",
    "PUBLISHING"
};

PUB_STATE publish_state;

char broker_username[64];
char broker_passwd[64];
char broker_ip[64];   
char topic[64];
int broker_port;
int loop_interval;

char *ds18b20[MAX_DS18B20];
int ds18b20_count;

float last_temp[MAX_DS18B20];

typedef struct
{
    int running;
    struct timeval tv;
} TIMER_TYPE;

TIMER_TYPE timers[MAX_TIMERS];

/* Initialise all timers - mark each as not running */
void init_timers()
{
    int c;

    for(c = 0; c < MAX_TIMERS; c++)
    {
        timers[c].running = false;
    }
}

/* Return true if timer is running, false if not */
int timer_running(int timer_id)
{
    return timers[timer_id].running;
}

/* Disable timer */
void cancel_timer(int timer_id)
{
    timers[timer_id].running = false;
}

/* Set timer to value given and enable it */
void set_timer(int timer_id, int seconds)
{
    gettimeofday(&timers[timer_id].tv, NULL);

    timers[timer_id].tv.tv_sec = timers[timer_id].tv.tv_sec + seconds;
    timers[timer_id].running = true;
}

/* Return true if timer has expired (or not runnning), false if not */
int check_timer(int timer_id)
{
    struct timeval tv;

    if(timers[timer_id].running == false)
    {
        return true;
    }

    gettimeofday(&tv, NULL);

    if(tv.tv_sec > timers[timer_id].tv.tv_sec)
    {
        timers[timer_id].running = false;
        return true;
    }

    if(tv.tv_sec == timers[timer_id].tv.tv_sec &&
        tv.tv_usec > timers[timer_id].tv.tv_usec)
    {
        timers[timer_id].running = false;
        return true;
    }

    return false;
}

/* Convert a state as used by state machine in main() to a string */
/* for syslog messages that need it */
char *state_to_name(PUB_STATE state)
{
    return state_names[state];
}

/* Set publishing timer for next publish event */
void schedule_next_publish()
{
    set_timer(TIMER_PUB, loop_interval);
}

/* return true if ready to publish */
int check_next_publish()
{
    return check_timer(TIMER_PUB);
}

/* Find all the temerature sensors and return the number found */
int find_ds18b20()
{
    DIR *dirp;
    struct dirent *direntp;
    char path[50];
    int device;

    bzero(ds18b20, sizeof(ds18b20));
    sprintf(path, "%s", W1_DIR);

    dirp = opendir(path);
    if(dirp == NULL)
    {
        return -1;
    }

    device = 0;

    /* Reads the directories or files in the current directory. */
    while((direntp = readdir(dirp)) != NULL && device < MAX_DS18B20)
    {
        // If 28- is the substring of d_name,
        // then save the name  
        if(strstr(direntp -> d_name, "28-"))
        {
            ds18b20[device] = malloc(32);
            if(ds18b20[device] == NULL)
            {
                syslog(LOG_ERR, "malloc error");
                return -1;
            }

            strcpy(ds18b20[device], direntp -> d_name);

            device++;
        }
    }
    closedir(dirp);

    // Return number of devices found on bus
    return device;
}

/* Read temperature from a DS18B20 sensor */
int get_temp(int device, float *temp)
{
    char path[50];
    int fd;
    char *temp_ptr;
    char buf[100];

    sprintf(path, "%s/%s/w1_slave", W1_DIR, ds18b20[device]);
    fd = open(path,O_RDONLY);
    if(fd < 0)
    {
        syslog(LOG_ERR, "open error");
        return -1;
    }

    if(read(fd, buf, sizeof(buf)) < 0)
    {
        syslog(LOG_ERR, "read error");
        return -1;
    }

    // Returns the first index of 't'.
    temp_ptr = strchr(buf,'t');

    // Read the string following "t=".
    sscanf(temp_ptr, "t=%s", temp_ptr);

    // convert to a floating point number
    *temp = atof(temp_ptr) / 1000;

    close(fd);

    return 0;
}

/* Get temperatures and format JSON message */
void get_data(char *pub_data)
{
    int device;
    char buf[80];
    float temp;

#ifdef __PUBLISH_TIMESTAMP
    time_t t;

    time(&t);
    sprintf(pub_data, "{\"dateTime\": \"%ld\", ", t);
#else
    sprintf(pub_data, "{");
#endif

    // For each sensor...
    device = 0;
    do
    {
        // Read temperature sensor
        get_temp(device, &temp);

        // First time through, use the value read
        if(last_temp[device] == INVALID_TEMP)
        {
            last_temp[device] = temp;
        }
        else
        {
            // If the value read is very different from the last one
            // then discard it and use the previous value instead
            // otherwise use the value just read
            if(fabs((double)(last_temp[device] - temp)) > MAX_TEMP_CHANGE)
            {
                syslog(LOG_INFO, "ignored ridiculous temperature - %f", temp);
                temp = last_temp[device];
            }
            else
            {
                last_temp[device] = temp;
            }
        }

        sprintf(buf, "\"extraTemp%d\": \"%0.1f\"", device + 1, temp);
        strcat(pub_data, buf);

        device++;
        if(device == ds18b20_count)
        {
            strcat(pub_data, "}");
        }
        else
        {
            strcat(pub_data, ", ");
        }
    } 
    while(device < ds18b20_count);
}

/* Publish data to broker */
void publish_loop(struct mosquitto *mos)
{
    char pub_data[255];
    int mid;
    int rc;

    get_data(pub_data);

    syslog(LOG_INFO, "sending data - %s", pub_data);

    if(mosquitto_publish(mos,
                      &mid,
                      topic,
                      strlen(pub_data),
                      pub_data,
                      2,
                      true) == MOSQ_ERR_SUCCESS)
    {
        publish_state = PUBLISHING;
    }
    else
    {
        syslog(LOG_ERR, "error publishing - %d", rc);
        mosquitto_disconnect(mos);
        publish_state = INIT;
    }
}

/* Called when connection made to broker */
void cb_connect(struct mosquitto *mos, void *obj, int rc)
{
    if(rc == 0)
    {
        syslog(LOG_INFO, "connected");
        publish_loop(mos);
    }
    else
    {
        syslog(LOG_ERR, "error connecting - %d", rc);
        publish_state = INIT;
    }

}

/* Called when data is published to broker */
void cb_publish(struct mosquitto *mos, void *obj, int mid)
{
    syslog(LOG_INFO, "published");
    mosquitto_disconnect(mos);
    publish_state = DISCONNECTING;
}

/* Called when disconnected from broker */
void cb_disconnect(struct mosquitto *mos, void *obj, int rc)
{
    syslog(LOG_INFO, "disconnected");
    publish_state = DISCONNECTED;
    schedule_next_publish();
}

int check_watchdog(struct mosquitto *mos)
{
    if(timer_running(TIMER_WDOG) == false)
    {
        set_timer(TIMER_WDOG, WATCHDOG_TIMEOUT);
        return false;
    }

    if(check_timer(TIMER_WDOG) == false)
    {
        return false;
    }

    cancel_timer(TIMER_WDOG);

    syslog(LOG_INFO, "watchdog timed out from state %s (%d)",
                                  state_to_name(publish_state), publish_state);

    mosquitto_disconnect(mos);

    return true; 
}

float init_temps()
{
    int c;

    for(c = 0; c < MAX_DS18B20; c++)
    {
        last_temp[c] = INVALID_TEMP;
    }
}

int main(int argc, char *argv[])
{
    struct mosquitto *mos;
    int c;

    openlog("publisher", LOG_PID, LOG_USER);

    init_timers();

    loop_interval = LOOP_INTERVAL;
    strcpy(broker_username, BROKER_USERNAME);
    strcpy(broker_passwd, BROKER_PASSWD);
    strcpy(broker_ip, BROKER_IP);
    broker_port = BROKER_PORT;
    strcpy(topic, TOPIC);

    while((c = getopt(argc, argv, "hl:u:p:a:x:t:")) != -1)
    {
        switch(c)
        {
            case 'l':
                loop_interval = atoi(optarg);
                break;

            case 'u':
                strncpy(broker_username, optarg, 32);
                break;

            case 'p':
                strncpy(broker_passwd, optarg, 32);
                break;

            case 'a':
                strncpy(broker_ip, optarg, 32);
                break;

            case 't':
                strncpy(topic, optarg, 32);
                break;

            case 'x':
                broker_port = atoi(optarg);
                break;

            case 'h':;
            case '?':
                printf("mqtt temperature publisher\n");
                printf("  -l <n>      loop interval in seconds\n");
                printf("  -u <user>   set broker username\n");
                printf("  -p <pass>   set broker password\n");
                printf("  -a <host>   set broker hostname\n");
                printf("  -t <topic>  set publishing topic\n");
                printf("  -x <n>      set broker port\n");
                return -1;

             default:
                return -1;
        }
    }

    syslog(LOG_ERR, "mqtt temperature publisher");
    syslog(LOG_ERR, "  broker: %s", broker_ip);
    syslog(LOG_ERR, "  user  : %s", broker_username);
    syslog(LOG_ERR, "  pass  : %s", broker_passwd);
    syslog(LOG_ERR, "  port  : %d", broker_port);
    syslog(LOG_ERR, "  topic : %s", topic);
    syslog(LOG_ERR, "  interv: %d", loop_interval);

    ds18b20_count = find_ds18b20();
    if(ds18b20_count < 1)
    {
        syslog(LOG_ERR, "couldn't find temperature sensors");
        return -1;
    }

    syslog(LOG_ERR,"found %d sensors", ds18b20_count);

    init_temps();

    /* initialise library */
    mosquitto_lib_init();

    /* Create new client */
    mos = mosquitto_new(CLIENT_ID, true, NULL);
    if(mos == NULL)
    {
        syslog(LOG_ERR, "mosquitto_new() failed");
        return -1;
    }

    /* Set username to username and password for broker */
    mosquitto_username_pw_set(mos, broker_username, broker_passwd);

    /* Setup callback function pointers */
    mosquitto_connect_callback_set(mos, cb_connect);
    mosquitto_publish_callback_set(mos, cb_publish);
    mosquitto_disconnect_callback_set(mos, cb_disconnect);

    publish_state = INIT;

    while(1)
    {
        switch(publish_state)
        {
            /* Open connection to broker */
            case INIT: 
                syslog(LOG_INFO, "connecting... ");
                if(mosquitto_connect(mos, broker_ip, broker_port, 10)
                                                          != MOSQ_ERR_SUCCESS)
                {
                    syslog(LOG_ERR, "mosquitto_connect() failed");
                }

                // if failed - wait for timeout and try again...
                publish_state = CONNECTING;
                break;

            /* Wait for connection to open or timeout */
            /* Connection will trigger publication and disconnection */
            case CONNECTING:;
                syslog(LOG_INFO, "CONNECTING");
                if(check_watchdog(mos) == true)
                {
                    publish_state = INIT;
                }
                break;

            /* Wait for disconnection or timeout */ 
            case DISCONNECTING:;
                syslog(LOG_INFO, "DISCONNECTING");
                if(check_watchdog(mos) == true)
                {
                    publish_state = INIT;
                }
                break;

            /* Wait for publication or timeout */
            case PUBLISHING:;
                syslog(LOG_INFO, "PUBLISHING");
                if(check_watchdog(mos) == true)
                {
                    publish_state = INIT;
                }
                break; 

            /* Wait for next publication time */
            case DISCONNECTED:
                syslog(LOG_INFO, "DISCONNECTED");
                if(check_next_publish() == true)
                {
                    publish_state = INIT;
                }
                break;

            default:;
                syslog(LOG_ERR, "invalid state - %d", publish_state);
        }

        mosquitto_loop(mos, MOS_BLOCKTIME, 1);
    }

    return 0;
}
