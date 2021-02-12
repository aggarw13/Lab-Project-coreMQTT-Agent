/*
 * FreeRTOS V202011.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file ota_over_mqtt_demo.c
 * @brief Over The Air Update demo using coreMQTT Agent.
 *
 * The file demonstrates how to perform Over The Air update using OTA agent and coreMQTT
 * library. It creates an OTA agent task which manages the OTA firmware update
 * for the device. The example also provides implementations to subscribe, publish,
 * and receive data from an MQTT broker. The implementation uses coreMQTT agent which manages
 * thread safety of the MQTT operations and allows OTA agent to share the same MQTT
 * broker connection with other tasks. OTA agent invokes the callback implementations to
 * publish job related control information, as well as receive chunks
 * of pre-signed firmware image from the MQTT broker.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html
 * See https://freertos.org/ota/ota-mqtt-agent-demo.html
 */

/* Standard includes. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "ota_config.h"
#include "demo_config.h"

/* MQTT library includes. */
#include "freertos_mqtt_agent.h"

/* Subscription manager header include. */
#include "subscription_manager.h"

/* OTA Library include. */
#include "ota.h"

/* Jobs library include. */
#include "jobs.h"

/* coreJSON library include. */
#include "core_json.h"

/* OTA Library Interface include. */
#include "ota_os_freertos.h"
#include "ota_mqtt_interface.h"
#include "ota_platform_interface.h"

/* Include firmware version struct definition. */
#include "ota_appversion32.h"

/* Include platform abstraction header. */
#include "ota_pal.h"

/*------------- Demo configurations -------------------------*/

#ifndef democonfigCLIENT_IDENTIFIER
    #error "Please define the democonfigCLIENT_IDENTIFIER with the thing name for which OTA is performed"
#endif

/**
 * @brief The maximum size of the file paths used in the demo.
 */
#define otaexampleMAX_FILE_PATH_SIZE      ( 260 )

/**
 * @brief The maximum size of the stream name required for downloading update file
 * from streaming service.
 */
#define otaexampleMAX_STREAM_NAME_SIZE    ( 128 )

/**
 * @brief The delay used in the OTA demo task to periodically output the OTA
 * statistics like number of packets received, dropped, processed and queued per connection.
 */
#define otaexampleTASK_DELAY_MS           ( 1000U )

/**
 * @brief The maximum time for which OTA demo waits for an MQTT operation to be complete.
 * This involves receiving an acknowledgment for broker for SUBSCRIBE, UNSUBSCRIBE and non
 * QOS0 publishes.
 */
#define otaexampleMQTT_TIMEOUT_MS         ( 5000U )


/**
 * @brief The common prefix string for all OTA topics.
 */
#define OTA_TOPIC_PREFIX                   "$aws/things/"

/**
 * @brief The length of topic prefix string #OTA_TOPIC_PREFIX
 */
#define OTA_TOPIC_PREFIX_LENGTH            ( ( uint16_t ) ( sizeof( OTA_TOPIC_PREFIX ) - 1U ) )

/**
 * @brief The sub string used to match jobs topics.
 */
#define OTA_TOPIC_JOBS                     "jobs"

/**
 * @brief Used to clear bits in a task's notification value.
 */
#define otaexampleMAX_UINT32               ( 0xffffffff )

/**
 * @brief The sub string used to match data stream topics.
 */
#define OTA_TOPIC_STREAM                   "streams"

/**
 * @brief Task priority of OTA agent.
 */
#define otaexampleAGENT_TASK_PRIORITY      ( tskIDLE_PRIORITY + 1 )

/**
 * @brief Maximum stack size of OTA agent task.
 */
#define otaexampleAGENT_TASK_STACK_SIZE    ( 4096 )

/**
 * @brief The version for the firmware which is running. OTA agent uses this
 * version number to perform anti-rollback validation. The firmware version for the
 * download image should be higher than the current version, otherwise the new image is
 * rejected in self test phase.
 */
#define APP_VERSION_MAJOR                  0
#define APP_VERSION_MINOR                  9
#define APP_VERSION_BUILD                  2

/**
 * @brief Defines the structure to keep the subscription info so that it can be accessed
 * in the command complete callback in this demo.
 */
typedef struct SubscriptionInfo
{
    IncomingPubCallback_t pxIncomingPublishCallback;
    MQTTAgentSubscribeArgs_t * pxSubscribeArgs;
    BaseType_t isSubscribe;
} SubscriptionInfo_t;

/**
 * @brief Defines the structure to use as the command callback context in this
 * demo.
 */
struct CommandContext
{
    MQTTStatus_t xReturnStatus;
    TaskHandle_t xTaskToNotify;
    void * pArgs;
};

/**
 * @brief Function used by OTA agent to publish control messages to the MQTT broker.
 *
 * The implementation uses MQTT agent to queue a publish request. It then waits
 * for the request complete notification from the agent. The notification along with result of the
 * operation is sent back to the caller task using xTaksNotify API. For publishes involving QOS 1 and
 * QOS2 the operation is complete once an acknwoledgment (PUBACK) is received. OTA agent uses this function
 * to fetch new job, provide status update and send other control related messges to the MQTT broker.
 *
 * @param[in] pacTopic Topic to publish the control packet to.
 * @param[in] topicLen Length of the topic string.
 * @param[in] pMsg Message to publish.
 * @param[in] msgSize Size of the message to publish.
 * @param[in] qos Qos for the publish.
 * @return OtaMqttSuccess if successful. Appropirate error code otherwise.
 */
static OtaMqttStatus_t prvMQTTPublish( const char * const pacTopic,
                                       uint16_t topicLen,
                                       const char * pMsg,
                                       uint32_t msgSize,
                                       uint8_t qos );

/**
 * @brief Function used by OTA agent to subscribe for a control or data packet from the MQTT broker.
 *
 * The implementation queues a SUBSCRIBE request for the topic filter with the MQTT agent. It then waits for
 * a notification of the request completion. Notification will be sent back to caller task,
 * using xTaskNotify APIs. MQTT agent also stores a callback provided by this function with
 * the associated topic filter. The callback will be used to
 * route any data received on the matching topic to the OTA agent. OTA agent uses this function
 * to subscribe to all topic filters necessary for receiving job related control messages as
 * well as firmware image chunks from MQTT broker.
 *
 * @param[in] pTopicFilter The topic filter used to subscribe for packets.
 * @param[in] topicFilterLength Length of the topic filter string.
 * @param[in] ucQoS Intended qos value for the messages received on this topic.
 * @return OtaMqttSuccess if successful. Appropirate error code otherwise.
 */
static OtaMqttStatus_t prvMQTTSubscribe( const char * pTopicFilter,
                                         uint16_t topicFilterLength,
                                         uint8_t ucQoS );

/**
 * @brief Function is used by OTA agent to unsubscribe a topicfilter from MQTT broker.
 *
 * The implementation queues an UNSUBSCRIBE request for the topic filter with the MQTT agent. It then waits
 * for a successful completion of the request from the agent. Notification along with results of
 * operation is sent using xTaskNotify API to the caller task. MQTT agent also removes the topic filter
 * subscription from its memory so any future
 * packets on this topic will not be routed to the OTA agent.
 *
 * @param[in] pTopicFilter Topic filter to be unsubscibed.
 * @param[in] topicFilterLength Length of the topic filter.
 * @param[in] ucQos Qos value for the topic.
 * @return OtaMqttSuccess if successful. Appropirate error code otherwise.
 *
 */
static OtaMqttStatus_t prvMQTTUnsubscribe( const char * pTopicFilter,
                                           uint16_t topicFilterLength,
                                           uint8_t ucQoS );

/**
 * @brief Passed into MQTTAgent_Subscribe() and MQTTAgent_Unsubscribe() as the
 * callback to execute when the broker ACKs the SUBSCRIBE/UNSUBSCRIBE message.
 * Its implementation sends a notification to the task that called
 * MQTTAgent_Subscribe() or MQTTAgent_Unsubscribe() to let the task know the
 * SUBSCRIBE/UNSUBSCRIBE operation is completed.  It also sets the xReturnStatus
 * of the structure passed in as the command's context to the value of the
 * xReturnStatus parameter - which enables the task to check the status of the
 * operation.
 *
 * See https://freertos.org/mqtt/mqtt-agent-demo.html#example_mqtt_api_call
 *
 * @param[in] pxCommandContext Context of the initial command.
 * @param[in] pxReturnInfo Returned info from MQTT Agent.
 */
static void prvSubscriptionCommandCallback( void * pxCommandContext,
                                            MQTTAgentReturnInfo_t * pxReturnInfo );

/**
 * @brief Fetch an unused OTA event buffer from the pool.
 *
 * Demo uses a simple statically allocated array of fixed size event buffers. The
 * number of event buffers is configured by the param otaconfigMAX_NUM_OTA_DATA_BUFFERS
 * within ota_config.h. This function is used to fetch a free buffer from the pool for processing
 * by the OTA agent task. It uses a mutex for thread safe access to the pool.
 *
 * @return A pointer to an unusued buffer. NULL if there are no buffers available.
 */
static OtaEventData_t * prvOTAEventBufferGet( void );

/**
 * @brief Free an event buffer back to pool
 *
 * OTA demo uses a statically allocated array of fixed size event buffers . The
 * number of event buffers is configured by the param otaconfigMAX_NUM_OTA_DATA_BUFFERS
 * within ota_config.h. The function is used by the OTA application callback to free a buffer,
 * after OTA agent has completed processing with the event. The access to the pool is made thread safe
 * using a mutex.
 *
 * @param[in] pxBuffer Pointer to the buffer to be freed.
 */
static void prvOTAEventBufferFree( OtaEventData_t * const pxBuffer );

/**
 * @brief The function which runs the OTA agent task.
 *
 * The function runs the OTA Agent Event processing loop, which waits for
 * any events for OTA agent and process them. The loop never returns until the OTA agent
 * is shutdown. The tasks exits gracefully by freeing up all resources in the event of an
 *  OTA agent shutdown.
 *
 * @param[in] pvParam Any parameters to be passed to OTA agent task.
 */
static void prvOTAAgentTask( void * pvParam );


/**
 * @brief The function which runs the OTA demo task.
 *
 * The demo task initializes the OTA agent an loops until OTA agent is shutdown.
 * It reports OTA update statistics (which includes number of blocks received, processed and dropped),
 * at regular intervals.
 *
 * @param[in] pvParam Any parameters to be passed to OTA demo task.
 */
static void prvOTADemoTask( void * pvParam );

/**
 * @brief Callback invoked for firmware image chunks received from MQTT broker.
 *
 * Function gets invoked for the firmware image blocks received on OTA data stream topic.
 * The function is registered with MQTT agent's subscription manger along with the
 * topic filter for data stream. For each packet received, the
 * function fetches a free event buffer from the pool and queues the firmware image chunk for
 * OTA agent task processing.
 *
 * @param[in] pxSubscriptionContext Context which is passed unmodified from the MQTT agent.
 * @param[in] pPublishInfo Pointer to the structure containing the details of the MQTT packet.
 */
static void prvProcessIncomingData( void * pxSubscriptionContext,
                                    MQTTPublishInfo_t * pPublishInfo );

/**
 * @brief Callback invoked for job control messages from MQTT broker.
 *
 * Callback gets invoked for any OTA job related control messages from the MQTT broker.
 * The function is registered with MQTT agent's subscription manger along with the topic filter for
 * job stream. The function fetches a free event buffer from the pool and queues the appropirate event type
 * based on the control message received.
 *
 * @param[in] pxSubscriptionContext Context which is passed unmodified from the MQTT agent.
 * @param[in] pPublishInfo Pointer to the structure containing the details of MQTT packet.
 */
static void prvProcessIncomingJobMessage( void * pxSubscriptionContext,
                                          MQTTPublishInfo_t * pPublishInfo );

/**
 * @brief Buffer used to store the firmware image file path.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t updateFilePath[ otaexampleMAX_FILE_PATH_SIZE ];

/**
 * @brief Buffer used to store the code signing certificate file path.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t certFilePath[ otaexampleMAX_FILE_PATH_SIZE ];

/**
 * @brief Buffer used to store the name of the data stream.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t streamName[ otaexampleMAX_STREAM_NAME_SIZE ];

/**
 * @brief Buffer used decode the CBOR message from the MQTT payload.
 * Buffer is passed to the OTA agent during initialization.
 */
static uint8_t decodeMem[ ( 1U << otaconfigLOG2_FILE_BLOCK_SIZE ) ];

/**
 * @brief Application buffer used to store the bitmap for requesting firmware image
 * chunks from MQTT broker. Buffer is passed to the OTA agent during initialization.
 */
static uint8_t bitmap[ OTA_MAX_BLOCK_BITMAP_SIZE ];

/**
 * @brief A statically allocated array of event buffers used by the OTA agent.
 * Maximum number of buffers are determined by how many chunks are requested
 * by OTA agent at a time along with an extra buffer to handle control message.
 * The size of each buffer is determined by the maximum size of firmware image
 * chunk, and other metadata send along with the chunk.
 */
static OtaEventData_t eventBuffer[ otaconfigMAX_NUM_OTA_DATA_BUFFERS ] = { 0 };

/*
 * @brief Mutex used to manage thread safe access of OTA event buffers.
 */
static SemaphoreHandle_t xBufferSemaphore;

/**
 * @brief Static handle used for MQTT agent context.
 */
extern MQTTAgentContext_t xGlobalMqttAgentContext;

/**
 * @brief Structure containing all application allocated buffers used by the OTA agent.
 * Structure is passed to the OTA agent during initialization.
 */
static OtaAppBuffer_t otaBuffer =
{
    .pUpdateFilePath    = updateFilePath,
    .updateFilePathsize = otaexampleMAX_FILE_PATH_SIZE,
    .pCertFilePath      = certFilePath,
    .certFilePathSize   = otaexampleMAX_FILE_PATH_SIZE,
    .pStreamName        = streamName,
    .streamNameSize     = otaexampleMAX_STREAM_NAME_SIZE,
    .pDecodeMemory      = decodeMem,
    .decodeMemorySize   = ( 1U << otaconfigLOG2_FILE_BLOCK_SIZE ),
    .pFileBitmap        = bitmap,
    .fileBitmapSize     = OTA_MAX_BLOCK_BITMAP_SIZE
};


/**
 * @brief Enum for different type of OTA messages received.
 */
typedef enum OtaMessageType
{
    OtaMessageTypeJob = 0,
    OtaMessageTypeStream,
    OtaNumOfMessageType
} OtaMessageType_t;

/**
 * @brief Structure used for encoding firmware version.
 */
const AppVersion32_t appFirmwareVersion =
{
    .u.x.major = APP_VERSION_MAJOR,
    .u.x.minor = APP_VERSION_MINOR,
    .u.x.build = APP_VERSION_BUILD,
};

/**
 * @brief The static callbacks for the topic filter types.
 */
static IncomingPubCallback_t otaMessageCallback[ OtaNumOfMessageType ] = { prvProcessIncomingJobMessage, prvProcessIncomingData };
/*-----------------------------------------------------------*/

static void prvOTAEventBufferFree( OtaEventData_t * const pxBuffer )
{
    if( xSemaphoreTake( xBufferSemaphore, portMAX_DELAY ) == pdTRUE )
    {
        pxBuffer->bufferUsed = false;
        ( void ) xSemaphoreGive( xBufferSemaphore );
    }
    else
    {
        LogError( ( "Failed to get buffer semaphore." ) );
    }
}

/*-----------------------------------------------------------*/

static OtaEventData_t * prvOTAEventBufferGet( void )
{
    uint32_t ulIndex = 0;
    OtaEventData_t * pFreeBuffer = NULL;

    if( xSemaphoreTake( xBufferSemaphore, portMAX_DELAY ) == pdTRUE )
    {
        for( ulIndex = 0; ulIndex < otaconfigMAX_NUM_OTA_DATA_BUFFERS; ulIndex++ )
        {
            if( eventBuffer[ ulIndex ].bufferUsed == false )
            {
                eventBuffer[ ulIndex ].bufferUsed = true;
                pFreeBuffer = &eventBuffer[ ulIndex ];
                break;
            }
        }

        ( void ) xSemaphoreGive( xBufferSemaphore );
    }
    else
    {
        LogError( ( "Failed to get buffer semaphore." ) );
    }

    return pFreeBuffer;
}

/*-----------------------------------------------------------*/
static void prvOTAAgentTask( void * pvParam )
{
    OTA_EventProcessingTask( pvParam );
    vTaskDelete( NULL );
}

/*-----------------------------------------------------------*/
static OtaMessageType_t getOtaMessageType( const char * pTopicFilter,
                                           uint16_t topicFilterLength )
{
    int retStatus = EXIT_FAILURE;

    uint16_t stringIndex = 0U, fieldLength = 0U, i = 0U;
    OtaMessageType_t retMesageType = OtaNumOfMessageType;

    /* Lookup table for OTA message string. */
    static const char * const pOtaMessageStrings[ OtaNumOfMessageType ] =
    {
        OTA_TOPIC_JOBS,
        OTA_TOPIC_STREAM
    };

    /* Check topic prefix is valid.*/
    if( strncmp( pTopicFilter, OTA_TOPIC_PREFIX, ( size_t ) OTA_TOPIC_PREFIX_LENGTH ) == 0 )
    {
        stringIndex = OTA_TOPIC_PREFIX_LENGTH;

        retStatus = EXIT_SUCCESS;
    }

    /* Check if thing name is valid.*/
    if( retStatus == EXIT_SUCCESS )
    {
        retStatus = EXIT_FAILURE;

        /* Extract the thing name.*/
        for( ; stringIndex < topicFilterLength; stringIndex++ )
        {
            if( pTopicFilter[ stringIndex ] == ( char ) '/' )
            {
                break;
            }
            else
            {
                fieldLength++;
            }
        }

        if( fieldLength > 0 )
        {
            /* Check thing name.*/
            if( strncmp( &pTopicFilter[ stringIndex - fieldLength ],
                         democonfigCLIENT_IDENTIFIER,
                         ( size_t ) ( fieldLength ) ) == 0 )
            {
                stringIndex++;

                retStatus = EXIT_SUCCESS;
            }
        }
    }

    /* Check the message type from topic.*/
    if( retStatus == EXIT_SUCCESS )
    {
        fieldLength = 0;

        /* Extract the topic type.*/
        for( ; stringIndex < topicFilterLength; stringIndex++ )
        {
            if( pTopicFilter[ stringIndex ] == ( char ) '/' )
            {
                break;
            }
            else
            {
                fieldLength++;
            }
        }

        if( fieldLength > 0 )
        {
            for( i = 0; i < OtaNumOfMessageType; i++ )
            {
                /* check thing name.*/
                if( strncmp( &pTopicFilter[ stringIndex - fieldLength ],
                             pOtaMessageStrings[ i ],
                             ( size_t ) ( fieldLength ) ) == 0 )
                {
                    break;
                }
            }

            if( i < OtaNumOfMessageType )
            {
                retMesageType = i;
            }
        }
    }

    return retMesageType;
}

/*-----------------------------------------------------------*/



/*-----------------------------------------------------------*/

/**
 * @brief The OTA agent has completed the update job or it is in
 * self test mode. If it was accepted, we want to activate the new image.
 * This typically means we should reset the device to run the new firmware.
 * If now is not a good time to reset the device, it may be activated later
 * by your user code. If the update was rejected, just return without doing
 * anything and we will wait for another job. If it reported that we should
 * start test mode, normally we would perform some kind of system checks to
 * make sure our new firmware does the basic things we think it should do
 * but we will just go ahead and set the image as accepted for demo purposes.
 * The accept function varies depending on your platform. Refer to the OTA
 * PAL implementation for your platform in aws_ota_pal.c to see what it
 * does for you.
 *
 * @param[in] event Specify if this demo is running with the AWS IoT
 * MQTT server. Set this to `false` if using another MQTT server.
 * @param[in] pData Data associated with the event.
 * @return None.
 */
static void otaAppCallback( OtaJobEvent_t event,
                            const void * pData )
{
    OtaErr_t err = OtaErrUninitialized;

    switch( event )
    {
        case OtaJobEventActivate:
            LogInfo( ( "Received OtaJobEventActivate callback from OTA Agent." ) );

            /**
             * Activate the new firmware image immediately. Applications can choose to postpone
             * the activation to a later stage if needed.
             */
            err = OTA_ActivateNewImage();

            /**
             * Activation of the new image failed. This indicates an error that requires a follow
             * up through manual activation by resetting the device. The demo reports the error
             * and shuts down the OTA agent.
             */
            LogError( ( "New image activation failed." ) );

            /* Shutdown OTA Agent without waiting. */
            OTA_Shutdown( 0 );


            break;

        case OtaJobEventFail:

            /**
             * No user action is needed here. OTA agent handles the job failure event.
             */
            LogInfo( ( "Received an OtaJobEventFail notification from OTA Agent." ) );

            break;

        case OtaJobEventStartTest:

            /* This demo just accepts the image since it was a good OTA update and networking
             * and services are all working (or we would not have made it this far). If this
             * were some custom device that wants to test other things before validating new
             * image, this would be the place to kick off those tests before calling
             * OTA_SetImageState() with the final result of either accepted or rejected. */

            LogInfo( ( "Received OtaJobEventStartTest callback from OTA Agent." ) );

            err = OTA_SetImageState( OtaImageStateAccepted );

            if( err == OtaErrNone )
            {
                LogInfo( ( "New image validation succeeded in self test mode." ) );
            }
            else
            {
                LogError( ( "Failed to set image state as accepted with error %d.", err ) );
            }

            break;

        case OtaJobEventProcessed:

            LogDebug( ( "OTA Event processing completed. Freeing the event buffer to pool." ) );
            configASSERT( pData != NULL );
            prvOTAEventBufferFree( ( OtaEventData_t * ) pData );

            break;

        case OtaJobEventSelfTestFailed:
            LogDebug( ( "Received OtaJobEventSelfTestFailed callback from OTA Agent." ) );

            /* Requires manual activation of previous image as self-test for
             * new image downloaded failed.*/
            LogError( ( "OTA Self-test failed for new image. shutting down OTA Agent." ) );

            /* Shutdown OTA Agent. */
            OTA_Shutdown( 0 );

            break;

        default:
            LogWarn( ( "Received an unhandled callback event from OTA Agent, event = %d", event ) );

            break;
    }
}

/*-----------------------------------------------------------*/

static void prvProcessIncomingData( void * pxSubscriptionContext,
                                    MQTTPublishInfo_t * pPublishInfo )
{
    configASSERT( pPublishInfo != NULL );

    ( void ) pxSubscriptionContext;

    OtaEventData_t * pData;
    OtaEventMsg_t eventMsg = { 0 };

    LogDebug( ( "Received OTA image block, size %d.\n\n", pPublishInfo->payloadLength ) );

    configASSERT( pPublishInfo->payloadLength <= OTA_DATA_BLOCK_SIZE );

    pData = prvOTAEventBufferGet();

    if( pData != NULL )
    {
        memcpy( pData->data, pPublishInfo->pPayload, pPublishInfo->payloadLength );
        pData->dataLength = pPublishInfo->payloadLength;
        eventMsg.eventId = OtaAgentEventReceivedFileBlock;
        eventMsg.pEventData = pData;

        /* Send job document received event. */
        OTA_SignalEvent( &eventMsg );
    }
    else
    {
        LogError( ( "Error: No OTA data buffers available.\r\n" ) );
    }
}

/*-----------------------------------------------------------*/

static BaseType_t xDemoEncountered = pdFALSE;
static BaseType_t xExitActionJobReceived = pdFALSE;

/**
 * @brief Currently supported actions that a job document can specify.
 */
typedef enum JobActionType
{
    JOB_ACTION_PRINT,   /**< Print a message. */
    JOB_ACTION_PUBLISH, /**< Publish a message to an MQTT topic. */
    JOB_ACTION_EXIT,    /**< Exit the demo. */
    JOB_ACTION_UNKNOWN  /**< Unknown action. */
} JobActionType;

static JobActionType prvGetAction( const char * pcAction,
                                   size_t xActionLength )
{
    JobActionType xAction = JOB_ACTION_UNKNOWN;

    configASSERT( pcAction != NULL );

    if( strncmp( pcAction, "print", xActionLength ) == 0 )
    {
        xAction = JOB_ACTION_PRINT;
    }
    else if( strncmp( pcAction, "publish", xActionLength ) == 0 )
    {
        xAction = JOB_ACTION_PUBLISH;
    }
    else if( strncmp( pcAction, "exit", xActionLength ) == 0 )
    {
        xAction = JOB_ACTION_EXIT;
    }

    return xAction;
}


static void prvSendUpdateForJob( char * pcJobId,
                                 uint16_t usJobIdLength,
                                 const char * pcJobStatusReport )
{
    char pUpdateJobTopic[ JOBS_API_MAX_LENGTH( THING_NAME_LENGTH ) ];
    size_t ulTopicLength = 0;
    JobsStatus_t xStatus = JobsSuccess;

    configASSERT( ( pcJobId != NULL ) && ( usJobIdLength > 0 ) );
    configASSERT( pcJobStatusReport != NULL );

    /* Generate the PUBLISH topic string for the UpdateJobExecution API of AWS IoT Jobs service. */
    xStatus = Jobs_Update( pUpdateJobTopic,
                           sizeof( pUpdateJobTopic ),
                           democonfigCLIENT_IDENTIFIER,
                           democonfigCLIENT_IDENTIFIER_LENGTH,
                           pcJobId,
                           usJobIdLength,
                           &ulTopicLength );

    if( xStatus == JobsSuccess )
    {
        BaseType_t result;
        MQTTStatus_t mqttStatus = MQTTBadParameter;
        MQTTPublishInfo_t publishInfo = { 0 };
        TaskHandle_t xTaskHandle;
        uint32_t ulNotifiedValue;
        CommandInfo_t xCommandParams = { 0 };

        publishInfo.pTopicName = pUpdateJobTopic;
        publishInfo.topicNameLength = ulTopicLength;
        publishInfo.qos = MQTTQoS1;
        publishInfo.pPayload = pcJobStatusReport;
        publishInfo.payloadLength = strlen( pcJobStatusReport );

        xTaskHandle = xTaskGetCurrentTaskHandle();
        xTaskNotifyStateClear( NULL );

        xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
        xCommandParams.cmdCompleteCallback = prvCommandCallback;
        xCommandParams.pCmdCompleteCallbackContext = ( void * ) xTaskHandle;

        mqttStatus = MQTTAgent_Publish( &xGlobalMqttAgentContext,
                                        &publishInfo,
                                        &xCommandParams );

        /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
         * duration of the command. */
        if( mqttStatus == MQTTSuccess )
        {
            result = xTaskNotifyWait( 0, otaexampleMAX_UINT32, &ulNotifiedValue, pdMS_TO_TICKS( otaexampleMQTT_TIMEOUT_MS ) );

            if( result != pdTRUE )
            {
                mqttStatus = MQTTSendFailed;
            }
            else
            {
                mqttStatus = ( MQTTStatus_t ) ( ulNotifiedValue );
            }
        }
    }
    else
    {
        LogError( ( "Failed to generate Publish topic string for sending job update: "
                    "JobID=%.*s, NewStatePayload=%s",
                    usJobIdLength, pcJobId, pcJobStatusReport ) );
    }
}

static void prvProcessJobDocument( char * pcJobId,
                                   uint16_t usJobIdLength,
                                   char * pcJobDocument,
                                   uint16_t jobDocumentLength )
{
    char * pcAction = NULL;
    size_t uActionLength = 0U;
    JSONStatus_t xJsonStatus = JSONSuccess;

    configASSERT( pcJobId != NULL );
    configASSERT( usJobIdLength > 0 );
    configASSERT( pcJobDocument != NULL );
    configASSERT( jobDocumentLength > 0 );

    xJsonStatus = JSON_Search( pcJobDocument,
                               jobDocumentLength,
                               jobsexampleQUERY_KEY_FOR_ACTION,
                               jobsexampleQUERY_KEY_FOR_ACTION_LENGTH,
                               &pcAction,
                               &uActionLength );

    if( xJsonStatus != JSONSuccess )
    {
        LogError( ( "Job document schema is invalid. Missing expected \"action\" key in document." ) );
        prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "FAILED" ) );
    }
    else
    {
        JobActionType xActionType = JOB_ACTION_UNKNOWN;
        char * pcMessage = NULL;
        size_t ulMessageLength = 0U;

        xActionType = prvGetAction( pcAction, uActionLength );

        switch( xActionType )
        {
            case JOB_ACTION_EXIT:
                LogInfo( ( "Received job contains \"exit\" action. Updating state of demo." ) );
                xExitActionJobReceived = pdTRUE;
                prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "SUCCEEDED" ) );

                /* Terminate the MQTT Agent task and therefore, the application. */
                CommandInfo_t xCommandParams = { 0 };
                xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
                xCommandParams.cmdCompleteCallback = NULL;
                xCommandParams.pCmdCompleteCallbackContext = NULL;

                MQTTAgent_Terminate( &xGlobalMqttAgentContext, &xCommandParams );

                break;

            case JOB_ACTION_PRINT:
                LogInfo( ( "Received job contains \"print\" action." ) );

                xJsonStatus = JSON_Search( pcJobDocument,
                                           jobDocumentLength,
                                           jobsexampleQUERY_KEY_FOR_MESSAGE,
                                           jobsexampleQUERY_KEY_FOR_MESSAGE_LENGTH,
                                           &pcMessage,
                                           &ulMessageLength );

                if( xJsonStatus == JSONSuccess )
                {
                    /* Print the given message if the action is "print". */
                    LogInfo( ( "\r\n"
                               "/*-----------------------------------------------------------*/\r\n"
                               "\r\n"
                               "%.*s\r\n"
                               "\r\n"
                               "/*-----------------------------------------------------------*/\r\n"
                               "\r\n", ulMessageLength, pcMessage ) );
                    prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "SUCCEEDED" ) );
                }
                else
                {
                    LogError( ( "Job document schema is invalid. Missing \"message\" for \"print\" action type." ) );
                    prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "FAILED" ) );
                }

                break;

            case JOB_ACTION_PUBLISH:
                LogInfo( ( "Received job contains \"publish\" action." ) );
                char * pcTopic = NULL;
                size_t ulTopicLength = 0U;

                xJsonStatus = JSON_Search( pcJobDocument,
                                           jobDocumentLength,
                                           jobsexampleQUERY_KEY_FOR_TOPIC,
                                           jobsexampleQUERY_KEY_FOR_TOPIC_LENGTH,
                                           &pcTopic,
                                           &ulTopicLength );

                /* Search for "topic" key in the Jobs document.*/
                if( xJsonStatus != JSONSuccess )
                {
                    LogError( ( "Job document schema is invalid. Missing \"topic\" key for \"publish\" action type." ) );
                    prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "FAILED" ) );
                }
                else
                {
                    xJsonStatus = JSON_Search( pcJobDocument,
                                               jobDocumentLength,
                                               jobsexampleQUERY_KEY_FOR_MESSAGE,
                                               jobsexampleQUERY_KEY_FOR_MESSAGE_LENGTH,
                                               &pcMessage,
                                               &ulMessageLength );

                    /* Search for "message" key in Jobs document.*/
                    if( xJsonStatus == JSONSuccess )
                    {
                        /* Publish to the parsed MQTT topic with the message obtained from
                         * the Jobs document.*/
                        if( xPublishToTopic( &xMqttContext,
                                             pcTopic,
                                             ulTopicLength,
                                             pcMessage,
                                             ulMessageLength ) == pdFALSE )
                        {
                            /* Set global flag to terminate demo as PUBLISH operation to execute job failed. */
                            xDemoEncounteredError = pdTRUE;

                            LogError( ( "Failed to execute job with \"publish\" action: Failed to publish to topic. "
                                        "JobID=%.*s, Topic=%.*s",
                                        usJobIdLength, pcJobId, ulTopicLength, pcTopic ) );
                        }

                        prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "SUCCEEDED" ) );
                    }
                    else
                    {
                        LogError( ( "Job document schema is invalid. Missing \"message\" key for \"publish\" action type." ) );
                        prvSendUpdateForJob( pcJobId, usJobIdLength, MAKE_STATUS_REPORT( "FAILED" ) );
                    }
                }

                break;

            default:
                configPRINTF( ( "Received Job document with unknown action %.*s.",
                                uActionLength, pcAction ) );
                break;
        }
    }
}

static void prvNextJobHandler( MQTTPublishInfo_t * pxPublishInfo )
{
    configASSERT( pxPublishInfo != NULL );
    configASSERT( ( pxPublishInfo->pPayload != NULL ) && ( pxPublishInfo->payloadLength > 0 ) );

    /* Check validity of JSON message response from server.*/
    if( JSON_Validate( pxPublishInfo->pPayload, pxPublishInfo->payloadLength ) != JSONSuccess )
    {
        LogError( ( "Received invalid JSON payload from AWS IoT Jobs service" ) );
    }
    else
    {
        char * pcJobId = NULL;
        size_t ulJobIdLength = 0UL;

        /* Parse the Job ID of the next pending job execution from the JSON payload. */
        if( JSON_Search( ( char * ) pxPublishInfo->pPayload,
                         pxPublishInfo->payloadLength,
                         jobsexampleQUERY_KEY_FOR_JOB_ID,
                         jobsexampleQUERY_KEY_FOR_JOB_ID_LENGTH,
                         &pcJobId,
                         &ulJobIdLength ) != JSONSuccess )
        {
            LogWarn( ( "Failed to parse Job ID in message received from AWS IoT Jobs service: "
                       "IncomingTopic=%.*s, Payload=%.*s",
                       pxPublishInfo->topicNameLength, pxPublishInfo->pTopicName,
                       pxPublishInfo->payloadLength, pxPublishInfo->pPayload ) );
        }
        else
        {
            char * pcJobDocLoc = NULL;
            size_t ulJobDocLength = 0UL;

            configASSERT( ulJobIdLength < JOBS_JOBID_MAX_LENGTH );
            LogInfo( ( "Received a Job from AWS IoT Jobs service: JobId=%.*s",
                       ulJobIdLength, pcJobId ) );

            /* Copy the Job ID in the global buffer. This is done so that
             * the MQTT context's network buffer can be used for sending jobs
             * status updates to the AWS IoT Jobs service. */
            memcpy( usJobIdBuffer, pcJobId, ulJobIdLength );

            /* Search for the jobs document in the payload. */
            if( JSON_Search( ( char * ) pxPublishInfo->pPayload,
                             pxPublishInfo->payloadLength,
                             jobsexampleQUERY_KEY_FOR_JOBS_DOC,
                             jobsexampleQUERY_KEY_FOR_JOBS_DOC_LENGTH,
                             &pcJobDocLoc,
                             &ulJobDocLength ) != JSONSuccess )
            {
                LogWarn( ( "Failed to parse document of next job received from AWS IoT Jobs service: "
                           "Topic=%.*s, JobID=%.*s",
                           pxPublishInfo->topicNameLength, pxPublishInfo->pTopicName,
                           ulJobIdLength, pcJobId ) );
            }
            else
            {
                /* Copy the Job document in buffer. This is done so that the MQTT connection buffer can
                 * be used for sending jobs status updates to the AWS IoT Jobs service. */
                memcpy( usJobsDocumentBuffer, pcJobDocLoc, ulJobDocLength );

                /* Process the Job document and execute the job. */
                prvProcessJobDocument( usJobIdBuffer,
                                       ( uint16_t ) ulJobIdLength,
                                       usJobsDocumentBuffer,
                                       ulJobDocLength );
            }
        }
    }
}

static void prvProcessIncomingJobMessage( void * pxSubscriptionContext,
                                          MQTTPublishInfo_t * pPublishInfo )
{
    OtaEventData_t * pData;
    OtaEventMsg_t eventMsg = { 0 };

    ( void ) pxSubscriptionContext;

    configASSERT( pPublishInfo != NULL );

    LogInfo( ( "Received job message callback, size %d.\n\n", pPublishInfo->payloadLength ) );

    configASSERT( pPublishInfo->payloadLength <= OTA_DATA_BLOCK_SIZE );

    /* Determine if this is an OTA or a custom job. */
    JobsTopic_t api;
    char * pJobId = NULL;
    uint16_t jobIdLength;
    JobsStatus_t status = JobsSuccess;
    status = Jobs_MatchTopic( pPublishInfo->pTopicName,
                              pPublishInfo->topicNameLength,
                              democonfigCLIENT_IDENTIFIER,
                              sizeof( democonfigCLIENT_IDENTIFIER ) - 1U,
                              &api,
                              &pJobId,
                              &jobIdLength );
    configASSERT( status == JobsSuccess );

    /* If it is an OTA job, signal the OTA agent to process it. */
    if( strncmp( pJobId, "AFR_OTA", ( strlen( "AFR_OTA" ) == 0 ) ) )
    {
        pData = prvOTAEventBufferGet();

        if( pData != NULL )
        {
            memcpy( pData->data, pPublishInfo->pPayload, pPublishInfo->payloadLength );
            pData->dataLength = pPublishInfo->payloadLength;
            eventMsg.eventId = OtaAgentEventReceivedJobDocument;
            eventMsg.pEventData = pData;

            /* Send job document received event. */
            OTA_SignalEvent( &eventMsg );
        }
        else
        {
            LogError( ( "Error: No OTA data buffers available.\r\n" ) );
        }
    }
    /* If it is a custom job, use custom application code to process it. */
    else
    {
        /* Upon successful return, the messageType has been filled in. */
        if( ( api == JobsDescribeSuccess ) || ( api == JobsNextJobChanged ) )
        {
            /* Handler function to process payload. */
            prvNextJobHandler( pPublishInfo );
        }
        else if( api == JobsUpdateSuccess )
        {
            LogInfo( ( "Job update status request has been accepted by AWS Iot Jobs service." ) );
        }
        else if( api == JobsStartNextFailed )
        {
            LogWarn( ( "Request for next job description rejected: RejectedResponse=%.*s.",
                       pPublishInfo->payloadLength,
                       ( const char * ) pPublishInfo->pPayload ) );
        }
        else if( api == JobsUpdateFailed )
        {
            LogWarn( ( "Request for job update rejected: RejectedResponse=%.*s.",
                       pPublishInfo->payloadLength,
                       ( const char * ) pPublishInfo->pPayload ) );

            LogError( ( "Terminating demo as request to update job status has been rejected by "
                        "AWS IoT Jobs service..." ) );
        }
        else
        {
            LogWarn( ( "Received an unexpected messages from AWS IoT Jobs service: "
                       "JobsTopicType=%u", topicType ) );
        }
    }
}

/*-----------------------------------------------------------*/

static void prvCommandCallback( void * pCommandContext,
                                MQTTAgentReturnInfo_t * pReturnInfo )
{
    TaskHandle_t xTaskToNotify = ( TaskHandle_t ) pCommandContext;

    configASSERT( xTaskToNotify );
    xTaskNotify( xTaskToNotify, pReturnInfo->returnCode, eSetValueWithOverwrite );
}

/*-----------------------------------------------------------*/

static void prvSubscriptionCommandCallback( void * pxCommandContext,
                                            MQTTAgentReturnInfo_t * pxReturnInfo )
{
    bool xSubscriptionAdded = false;
    CommandContext_t * pxApplicationDefinedContext = ( CommandContext_t * ) pxCommandContext;
    SubscriptionInfo_t * pxSubscriptionInfo = ( SubscriptionInfo_t * ) pxApplicationDefinedContext->pArgs;

    /* Store the result in the application defined context so the task that
     * initiated the subscribe can check the operation's status.  Also send the
     * status as the notification value.  These things are just done for
     * demonstration purposes. */
    pxApplicationDefinedContext->xReturnStatus = pxReturnInfo->returnCode;

    /* Check if the subscribe operation is a success. Only one topic at a time is
     * subscribed by this demo. */
    if( pxReturnInfo->returnCode == MQTTSuccess )
    {
        /* Subscribe. */
        if( pxSubscriptionInfo->isSubscribe == pdTRUE )
        {
            /* Add subscription so that incoming publishes are routed to the application
             * callback. */
            xSubscriptionAdded = addSubscription( ( SubscriptionElement_t * ) xGlobalMqttAgentContext.pIncomingCallbackContext,
                                                  pxSubscriptionInfo->pxSubscribeArgs->pSubscribeInfo->pTopicFilter,
                                                  pxSubscriptionInfo->pxSubscribeArgs->pSubscribeInfo->topicFilterLength,
                                                  pxSubscriptionInfo->pxIncomingPublishCallback,
                                                  NULL );

            if( xSubscriptionAdded == false )
            {
                LogError( ( "Failed to register an incoming publish callback for topic %.*s.",
                            pxSubscriptionInfo->pxSubscribeArgs->pSubscribeInfo->topicFilterLength,
                            pxSubscriptionInfo->pxSubscribeArgs->pSubscribeInfo->pTopicFilter ) );
            }
        }
        else
        {
            /* Remove subscription callback for unsubscribe. */
            removeSubscription( ( SubscriptionElement_t * ) xGlobalMqttAgentContext.pIncomingCallbackContext,
                                pxSubscriptionInfo->pxSubscribeArgs->pSubscribeInfo->pTopicFilter,
                                pxSubscriptionInfo->pxSubscribeArgs->pSubscribeInfo->topicFilterLength );
        }
    }

    xTaskNotify( pxApplicationDefinedContext->xTaskToNotify,
                 ( uint32_t ) ( pxReturnInfo->returnCode ),
                 eSetValueWithOverwrite );
}

/*-----------------------------------------------------------*/

static MQTTStatus_t prvSubscribeToTopic( MQTTQoS_t xQoS,
                                         const char * pcTopicFilter,
                                         IncomingPubCallback_t pCallback )
{
    MQTTStatus_t mqttStatus;
    uint32_t ulNotifiedValue;
    MQTTAgentSubscribeArgs_t xSubscribeArgs = { 0 };
    MQTTSubscribeInfo_t xSubscribeInfo = { 0 };
    BaseType_t result;
    CommandInfo_t xCommandParams = { 0 };
    CommandContext_t xApplicationDefinedContext = { 0 };
    SubscriptionInfo_t xSubscriptionInfo = { 0 };

    TaskHandle_t xTaskHandle = xTaskGetCurrentTaskHandle();

    xSubscribeInfo.pTopicFilter = pcTopicFilter;
    xSubscribeInfo.topicFilterLength = ( uint16_t ) strlen( pcTopicFilter );
    xSubscribeInfo.qos = xQoS;
    xSubscribeArgs.pSubscribeInfo = &xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 1;

    /* Complete an application defined context associated with this subscribe message.
     * This gets updated in the callback function so the variable must persist until
     * the callback executes. */
    xSubscriptionInfo.isSubscribe = pdTRUE;
    xSubscriptionInfo.pxSubscribeArgs = &xSubscribeArgs;
    xSubscriptionInfo.pxIncomingPublishCallback = pCallback;

    xApplicationDefinedContext.xTaskToNotify = xTaskHandle;
    xApplicationDefinedContext.pArgs = ( void * ) &xSubscriptionInfo;

    xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
    xCommandParams.cmdCompleteCallback = prvSubscriptionCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = ( void * ) &xApplicationDefinedContext;

    LogInfo( ( " Subscribing to topic filter: %s", pcTopicFilter ) );
    xTaskNotifyStateClear( NULL );

    mqttStatus = MQTTAgent_Subscribe( &xGlobalMqttAgentContext,
                                      &xSubscribeArgs,
                                      &xCommandParams );

    /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
     * duration of the command. */
    if( mqttStatus == MQTTSuccess )
    {
        result = xTaskNotifyWait( 0, otaexampleMAX_UINT32, &ulNotifiedValue, pdMS_TO_TICKS( otaexampleMQTT_TIMEOUT_MS ) );

        if( result == pdTRUE )
        {
            mqttStatus = ( MQTTStatus_t ) ulNotifiedValue;
        }
        else
        {
            mqttStatus = MQTTRecvFailed;
        }
    }

    return mqttStatus;
}

static OtaMqttStatus_t prvMQTTSubscribe( const char * pTopicFilter,
                                         uint16_t topicFilterLength,
                                         uint8_t ucQoS )
{
    OtaMqttStatus_t otaRet = OtaMqttSuccess;
    OtaMessageType_t otaMessageType;
    MQTTStatus_t mqttStatus = MQTTBadParameter;

    configASSERT( pTopicFilter != NULL );
    configASSERT( topicFilterLength > 0 );

    otaMessageType = getOtaMessageType( pTopicFilter, topicFilterLength );
    configASSERT( otaMessageType < OtaNumOfMessageType );

    /* Send SUBSCRIBE packet. */
    mqttStatus = prvSubscribeToTopic( ucQoS,
                                      pTopicFilter,
                                      otaMessageCallback[ otaMessageType ] );

    if( mqttStatus != MQTTSuccess )
    {
        LogError( ( "Failed to SUBSCRIBE to topic with error = %u.",
                    mqttStatus ) );

        otaRet = OtaMqttSubscribeFailed;
    }
    else
    {
        LogInfo( ( "SUBSCRIBE topic %.*s to broker.\n\n",
                   topicFilterLength,
                   pTopicFilter ) );

        otaRet = OtaMqttSuccess;
    }

    return otaRet;
}

static OtaMqttStatus_t prvMQTTPublish( const char * const pacTopic,
                                       uint16_t topicLen,
                                       const char * pMsg,
                                       uint32_t msgSize,
                                       uint8_t qos )
{
    OtaMqttStatus_t otaRet = OtaMqttSuccess;
    BaseType_t result;
    MQTTStatus_t mqttStatus = MQTTBadParameter;
    MQTTPublishInfo_t publishInfo = { 0 };
    TaskHandle_t xTaskHandle;
    uint32_t ulNotifiedValue;
    CommandInfo_t xCommandParams = { 0 };

    publishInfo.pTopicName = pacTopic;
    publishInfo.topicNameLength = topicLen;
    publishInfo.qos = qos;
    publishInfo.pPayload = pMsg;
    publishInfo.payloadLength = msgSize;

    xTaskHandle = xTaskGetCurrentTaskHandle();
    xTaskNotifyStateClear( NULL );

    xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
    xCommandParams.cmdCompleteCallback = prvCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = ( void * ) xTaskHandle;

    mqttStatus = MQTTAgent_Publish( &xGlobalMqttAgentContext,
                                    &publishInfo,
                                    &xCommandParams );

    /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
     * duration of the command. */
    if( mqttStatus == MQTTSuccess )
    {
        result = xTaskNotifyWait( 0, otaexampleMAX_UINT32, &ulNotifiedValue, pdMS_TO_TICKS( otaexampleMQTT_TIMEOUT_MS ) );

        if( result != pdTRUE )
        {
            mqttStatus = MQTTSendFailed;
        }
        else
        {
            mqttStatus = ( MQTTStatus_t ) ( ulNotifiedValue );
        }
    }

    if( mqttStatus != MQTTSuccess )
    {
        LogError( ( "Failed to send PUBLISH packet to broker with error = %u.", mqttStatus ) );
        otaRet = OtaMqttPublishFailed;
    }
    else
    {
        LogInfo( ( "Sent PUBLISH packet to broker %.*s to broker.\n\n",
                   topicLen,
                   pacTopic ) );

        otaRet = OtaMqttSuccess;
    }

    return otaRet;
}

static MQTTStatus_t prvUnSubscribeFromTopic( MQTTQoS_t xQoS,
                                             const char * pcTopicFilter )
{
    MQTTStatus_t mqttStatus;
    uint32_t ulNotifiedValue;
    MQTTAgentSubscribeArgs_t xSubscribeArgs = { 0 };
    MQTTSubscribeInfo_t xSubscribeInfo = { 0 };
    BaseType_t result;
    CommandInfo_t xCommandParams = { 0 };
    CommandContext_t xApplicationDefinedContext = { 0 };
    SubscriptionInfo_t xSubscriptionInfo = { 0 };

    TaskHandle_t xTaskHandle = xTaskGetCurrentTaskHandle();

    xSubscribeInfo.pTopicFilter = pcTopicFilter;
    xSubscribeInfo.topicFilterLength = ( uint16_t ) strlen( pcTopicFilter );
    xSubscribeInfo.qos = xQoS;
    xSubscribeArgs.pSubscribeInfo = &xSubscribeInfo;
    xSubscribeArgs.numSubscriptions = 1;

    /* Complete an application defined context associated with this subscribe message.
     * This gets updated in the callback function so the variable must persist until
     * the callback executes. */
    xSubscriptionInfo.isSubscribe = pdFALSE;
    xSubscriptionInfo.pxSubscribeArgs = &xSubscribeArgs;

    xApplicationDefinedContext.xTaskToNotify = xTaskHandle;
    xApplicationDefinedContext.pArgs = ( void * ) &xSubscriptionInfo;

    xCommandParams.blockTimeMs = otaexampleMQTT_TIMEOUT_MS;
    xCommandParams.cmdCompleteCallback = prvSubscriptionCommandCallback;
    xCommandParams.pCmdCompleteCallbackContext = ( void * ) &xApplicationDefinedContext;

    LogInfo( ( " Unsubscribing to topic filter: %s", pcTopicFilter ) );
    xTaskNotifyStateClear( NULL );


    mqttStatus = MQTTAgent_Unsubscribe( &xGlobalMqttAgentContext,
                                        &xSubscribeArgs,
                                        &xCommandParams );

    /* Wait for command to complete so MQTTSubscribeInfo_t remains in scope for the
     * duration of the command. */
    if( mqttStatus == MQTTSuccess )
    {
        result = xTaskNotifyWait( 0, otaexampleMAX_UINT32, &ulNotifiedValue, pdMS_TO_TICKS( otaexampleMQTT_TIMEOUT_MS ) );

        if( result == pdTRUE )
        {
            mqttStatus = ( MQTTStatus_t ) ( ulNotifiedValue );
        }
        else
        {
            mqttStatus = MQTTRecvFailed;
        }
    }

    return mqttStatus;
}

static OtaMqttStatus_t prvMQTTUnsubscribe( const char * pTopicFilter,
                                           uint16_t topicFilterLength,
                                           uint8_t ucQoS )
{
    OtaMqttStatus_t otaRet = OtaMqttSuccess;
    MQTTStatus_t mqttStatus = MQTTBadParameter;

    configASSERT( pTopicFilter != NULL );
    configASSERT( topicFilterLength > 0 );

    /* Send SUBSCRIBE packet. */
    mqttStatus = prvUnSubscribeFromTopic( ucQoS,
                                          pTopicFilter );

    if( mqttStatus != MQTTSuccess )
    {
        LogError( ( "Failed to UNSUBSCRIBE from topic %.*s with error = %u.",
                    topicFilterLength,
                    pTopicFilter,
                    mqttStatus ) );

        otaRet = OtaMqttUnsubscribeFailed;
    }
    else
    {
        LogInfo( ( "UNSUBSCRIBED from topic %.*s.\n\n",
                   topicFilterLength,
                   pTopicFilter ) );

        otaRet = OtaMqttSuccess;
    }

    return otaRet;
}

/*-----------------------------------------------------------*/

static void setOtaInterfaces( OtaInterfaces_t * pOtaInterfaces )
{
    configASSERT( pOtaInterfaces != NULL );

    /* Initialize OTA library OS Interface. */
    pOtaInterfaces->os.event.init = OtaInitEvent_FreeRTOS;
    pOtaInterfaces->os.event.send = OtaSendEvent_FreeRTOS;
    pOtaInterfaces->os.event.recv = OtaReceiveEvent_FreeRTOS;
    pOtaInterfaces->os.event.deinit = OtaDeinitEvent_FreeRTOS;
    pOtaInterfaces->os.timer.start = OtaStartTimer_FreeRTOS;
    pOtaInterfaces->os.timer.stop = OtaStopTimer_FreeRTOS;
    pOtaInterfaces->os.timer.delete = OtaDeleteTimer_FreeRTOS;
    pOtaInterfaces->os.mem.malloc = Malloc_FreeRTOS;
    pOtaInterfaces->os.mem.free = Free_FreeRTOS;

    /* Initialize the OTA library MQTT Interface.*/
    pOtaInterfaces->mqtt.subscribe = prvMQTTSubscribe;
    pOtaInterfaces->mqtt.publish = prvMQTTPublish;
    pOtaInterfaces->mqtt.unsubscribe = prvMQTTUnsubscribe;

    /* Initialize the OTA library PAL Interface.*/
    pOtaInterfaces->pal.getPlatformImageState = otaPal_GetPlatformImageState;
    pOtaInterfaces->pal.setPlatformImageState = otaPal_SetPlatformImageState;
    pOtaInterfaces->pal.writeBlock = otaPal_WriteBlock;
    pOtaInterfaces->pal.activate = otaPal_ActivateNewImage;
    pOtaInterfaces->pal.closeFile = otaPal_CloseFile;
    pOtaInterfaces->pal.reset = otaPal_ResetDevice;
    pOtaInterfaces->pal.abort = otaPal_Abort;
    pOtaInterfaces->pal.createFile = otaPal_CreateFileForRx;
}

static void prvOTADemoTask( void * pvParam )
{
    ( void ) pvParam;
    /* FreeRTOS APIs return status. */
    BaseType_t xResult = pdPASS;

    /* OTA library return status. */
    OtaErr_t otaRet = OtaErrNone;

    /* OTA event message used for sending event to OTA Agent.*/
    OtaEventMsg_t eventMsg = { 0 };

    /* OTA interface context required for library interface functions.*/
    OtaInterfaces_t otaInterfaces;

    /* OTA library packet statistics per job.*/
    OtaAgentStatistics_t otaStatistics = { 0 };

    /* OTA Agent state returned from calling OTA_GetAgentState.*/
    OtaState_t state = OtaAgentStateStopped;

    /* Set OTA Library interfaces.*/
    setOtaInterfaces( &otaInterfaces );

    LogInfo( ( "OTA over MQTT demo, Application version %u.%u.%u",
               appFirmwareVersion.u.x.major,
               appFirmwareVersion.u.x.minor,
               appFirmwareVersion.u.x.build ) );
    /****************************** Init OTA Library. ******************************/

    xBufferSemaphore = xSemaphoreCreateMutex();

    if( xBufferSemaphore == NULL )
    {
        xResult = pdFAIL;
    }

    if( xResult == pdPASS )
    {
        memset( eventBuffer, 0x00, sizeof( eventBuffer ) );

        if( ( otaRet = OTA_Init( &otaBuffer,
                                 &otaInterfaces,
                                 ( const uint8_t * ) ( democonfigCLIENT_IDENTIFIER ),
                                 otaAppCallback ) ) != OtaErrNone )
        {
            LogError( ( "Failed to initialize OTA Agent, exiting = %u.",
                        otaRet ) );
            xResult = pdFAIL;
        }
    }

    if( xResult == pdPASS )
    {
        if( ( xResult = xTaskCreate( prvOTAAgentTask,
                                     "OTAAgentTask",
                                     otaexampleAGENT_TASK_STACK_SIZE,
                                     NULL,
                                     otaexampleAGENT_TASK_PRIORITY,
                                     NULL ) ) != pdPASS )
        {
            LogError( ( "Failed to start OTA Agent task: "
                        ",errno=%d",
                        xResult ) );
        }
    }

    /***************************Start OTA demo loop. ******************************/

    if( xResult == pdPASS )
    {
        /* Start the OTA Agent.*/
        eventMsg.eventId = OtaAgentEventStart;
        OTA_SignalEvent( &eventMsg );

        while( ( ( state = OTA_GetState() ) != OtaAgentStateStopped ) )
        {
            /* Get OTA statistics for currently executing job. */
            OTA_GetStatistics( &otaStatistics );
            LogInfo( ( " Received: %u   Queued: %u   Processed: %u   Dropped: %u",
                       otaStatistics.otaPacketsReceived,
                       otaStatistics.otaPacketsQueued,
                       otaStatistics.otaPacketsProcessed,
                       otaStatistics.otaPacketsDropped ) );

            vTaskDelay( pdMS_TO_TICKS( otaexampleTASK_DELAY_MS ) );
        }
    }

    LogInfo( ( "OTA agent task stopped. Exiting OTA demo." ) );

    vTaskDelete( NULL );
}

void vStartOTACodeSigningDemo( configSTACK_DEPTH_TYPE uxStackSize,
                               UBaseType_t uxPriority )
{
    BaseType_t xResult;

    if( ( xResult = xTaskCreate( prvOTADemoTask,
                                 "OTADemoTask",
                                 uxStackSize,
                                 NULL,
                                 uxPriority,
                                 NULL ) ) != pdPASS )
    {
        LogError( ( "Failed to start OTA task: "
                    ",errno=%d",
                    xResult ) );
    }

    configASSERT( xResult == pdPASS );
}
void vSuspendOTACodeSigningDemo( void )
{
    if( ( OTA_GetState() != OtaAgentStateSuspended ) && ( OTA_GetState() != OtaAgentStateStopped ) )
    {
        OTA_Suspend();

        while( ( OTA_GetState() != OtaAgentStateSuspended ) &&
               ( OTA_GetState() != OtaAgentStateStopped ) )
        {
            vTaskDelay( pdMS_TO_TICKS( otaexampleTASK_DELAY_MS ) );
        }
    }
}

void vResumeOTACodeSigningDemo( void )
{
    if( OTA_GetState() == OtaAgentStateSuspended )
    {
        OTA_Resume();

        while( OTA_GetState() == OtaAgentStateSuspended )
        {
            vTaskDelay( pdMS_TO_TICKS( otaexampleTASK_DELAY_MS ) );
        }
    }
}
