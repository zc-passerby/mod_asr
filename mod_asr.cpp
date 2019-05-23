#include <stdio.h>
#include <sys/time.h>

#include <string>
#include <ctime>
#include <sstream>

#include "switch.h"
#include "nlsClient.h"
#include "nlsEvent.h"
#include "speechTranscriberRequest.h"
#include "nlsCommonSdk/Token.h"

#define ASR_PRIVATE "_asr_"

using std::string;
using AlibabaNls::NlsClient;
using AlibabaNls::NlsEvent;
using AlibabaNls::SpeechTranscriberCallback;    // 阿里实时语音识别回调
using AlibabaNls::SpeechTranscriberRequest;     // 阿里实时语音识别句柄
using AlibabaNlsCommon::NlsToken;

/**
* 全局维护一个服务鉴权token和其对应的有效期时间戳，
* 每次调用服务之前，首先判断token是否已经过期，
* 如果已经过期，则根据AccessKey ID和AccessKey Secret重新生成一个token，并更新这个全局的token和其有效期时间戳。
*
* 注意：不要每次调用服务之前都重新生成新token，只需在token即将过期时重新生成即可。所有的服务并发可共用一个token。
*/
string g_token = "";
long g_expireTime = -1;

SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown);
SWITCH_STANDARD_APP(start_ali_asr_function);
SWITCH_STANDARD_APP(stop_ali_asr_function);



extern "C" {
    SWITCH_MODULE_DEFINITION(mod_asr, mod_asr_load, mod_asr_shutdown, NULL);
}

typedef struct
{
    switch_core_session_t       *asr_session;
    switch_media_bug_t          *read_bug;
    switch_audio_resampler_t    *read_resampler;

    SpeechTranscriberRequest    *request;
    SpeechTranscriberCallback   *callback;

    int                         sample_rate;

    char                        *aliAppKey;
    char                        *aliAccessKeyID;
    char                        *aliAccessKeySecret;
}switch_asr_t;

/**
* 根据AccessKey ID和AccessKey Secret重新生成一个token，并获取其有效期时间戳
*/
int generate_token(string akId, string akSecret, string* token, long* expireTime)
{
    NlsToken nlsTokenRequest;
    nlsTokenRequest.setAccessKeyId(akId);
    nlsTokenRequest.setKeySecret(akSecret);

    if (-1 == nlsTokenRequest.applyNlsToken())
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Failed to get token %s\n", nlsTokenRequest.getErrorMsg());
        return -1;
    }

    *token = nlsTokenRequest.getToken();
    *expireTime = nlsTokenRequest.getExpireTime();

    return 0;
}

// 识别启动回调函数
void onTranscriptionStarted(NlsEvent *str, void *param)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " onTranscriptionStarted %d %s\n", str->getStausCode(), str->getTaskId());
}

// 识别结果变化回调函数
void onTranscriptionResultChanged(NlsEvent *str, void *param)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " onTranscriptionResultChanged %d %s %s\n", str->getStausCode(), str->getTaskId(), str->getResult());
}

// 语音转写结束回调函数
void onTranscriptionCompleted(NlsEvent *str, void *param)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " onTranscriptionCompleted %s %s\n", str->getTaskId(), str->getResult());
}

// 一句话开始回调函数
void onSentenceBegin(NlsEvent *str, void *param)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = (switch_channel_t *)param;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " onSentenceBegin %d %s\n", str->getStausCode(), str->getTaskId());

    if(switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS)
    {
        event->subclass_name = strdup("aliasr::asr_start_talking");
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", event->subclass_name);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_channel_get_uuid(channel));
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AliAsr-Channel", str->getTaskId());
        switch_event_fire(event);
    }
}

// 一句话结束回调函数
void onSentenceEnd(NlsEvent *str, void *param)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = (switch_channel_t *)param;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " onSentenceEnd %s %f %s\n", str->getResult(), str->getSentenceConfidence(), str->getAllResponse());

    if(switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS)
    {
        event->subclass_name = strdup("aliasr::asr_stop_talking");
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", event->subclass_name);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_channel_get_uuid(channel));
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AliAsr-Channel", str->getTaskId());
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AliAsr-Result", str->getResult());
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AliAsr-Sentence-Confidence", str->getSentenceConfidence());
        switch_event_fire(event);
    }
}

// 异常识别回调函数
void onTaskFailed(NlsEvent *str, void *param)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = (switch_channel_t *)param;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, " onTaskFailed %s %s\n", str->getTaskId(), str->getErrorMessage());

    if(switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS)
    {
        event->subclass_name = strdup("aliasr::asr_task_failed");
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", event->subclass_name);
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_channel_get_uuid(channel));
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AliAsr-Channel", str->getTaskId());
        switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "AliAsr-Result", str->getErrorMessage());
        switch_event_fire(event);
    }
}

// 识别通道关闭回调函数
void onChannelClosed(NlsEvent *str, void *param)
{
    switch_event_t *event = NULL;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " onChannelClosed %s %s\n", str->getTaskId(), str->getAllResponse());
}

// switch_core_media_bug_add添加的回调函数，有语音流就会调用这个函数
static switch_bool_t asr_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    switch_asr_t *pvt = (switch_asr_t *)user_data;
    switch_core_session_t *session = pvt->asr_session;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (!switch_channel_media_ready(channel))
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel codec isn't ready\n");
        return SWITCH_TRUE;
    }

    switch(type)
    {
        case SWITCH_ABC_TYPE_INIT:
        {
            switch_codec_implementation_t read_impl;

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Starting ASR detection for audio stream\n");

            /*
             * 创建实时音频流识别SpeechTranscriberRequest对象, 参数为callback对象.
             * request对象在一个会话周期内可以重复使用.
             * 会话周期是一个逻辑概念. 比如Demo中, 指读取, 发送完整个音频文件数据的时间.
             * 音频文件数据发送结束时, 可以releaseTranscriberRequest()释放对象.
             * createTranscriberRequest(), start(), sendAudio(), stop(), releaseTranscriberRequest()请在
             * 同一线程内完成, 跨线程使用可能会引起异常错误.
             */
            /*
             * 2: 创建实时音频流识别SpeechTranscriberRequest对象
             */
            SpeechTranscriberRequest *request = NlsClient::getInstance()->createTranscriberRequest(pvt->callback);
            if (request == NULL)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s Failed to create SpeechTranscriberRequest.\n", switch_channel_get_name(channel));
                delete pvt->callback;
                pvt->callback = NULL;
                break;
            }
            //request->setAppKey("TYMHcrI1SraL2JaE"); // 设置AppKey, 必填参数, 请参照官网申请
            request->setAppKey(pvt->aliAppKey); // 设置AppKey, 必填参数, 请参照官网申请
            request->setFormat("pcm"); // 设置音频数据编码格式, 可选参数, 目前支持pcm, opu, opus, speex. 默认是pcm
            request->setSampleRate(pvt->sample_rate); // 设置音频数据采样率, 可选参数，目前支持16000, 8000. 默认是16000
            request->setIntermediateResult(false); // 设置是否返回中间识别结果, 可选参数. 默认false
            request->setPunctuationPrediction(true); // 设置是否在后处理中添加标点, 可选参数. 默认false
            request->setInverseTextNormalization(true); // 设置是否在后处理中执行数字转写, 可选参数. 默认false

            //语音断句检测阈值，一句话之后静音长度超过该值，即本句结束，合法参数范围200～2000(ms)，默认值800ms
            //request->setMaxSentenceSilence(800);
            //request->setCustomizationId("TestId_123"); //定制模型id, 可选.
            //request->setVocabularyId("TestId_456"); //定制泛热词id, 可选.

            request->setToken(g_token.c_str());

            /*
             * 3: start()为阻塞操作, 发送start指令之后, 会等待服务端响应, 或超时之后才返回
             */
            if (request->start() < 0)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s createTranscriberRequest start() failed. \n", switch_channel_get_name(channel));
                NlsClient::getInstance()->releaseTranscriberRequest(request); // start()失败，释放request对象
                delete pvt->callback;
                pvt->callback = NULL;
                break;
            }
            else
            {
                pvt->request = request;
                switch_core_session_get_read_impl(session, &read_impl);
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Read imp %u %u.\n", read_impl.samples_per_second, read_impl.number_of_channels);

                if(read_impl.actual_samples_per_second != 8000) // 采样率不是8KHz则将采样率调整为8KHz
                {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " resample the audio. \n");
                    switch_resample_create(&pvt->read_resampler, read_impl.actual_samples_per_second, 8000, 320, SWITCH_RESAMPLE_QUALITY, 1);
                }
            }
            break;
        }
        case SWITCH_ABC_TYPE_CLOSE:
        {
            if(pvt->request)
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Stop Succeed channel:%s\n", switch_channel_get_name(channel));
                pvt->request->stop();
                NlsClient::getInstance()->releaseTranscriberRequest(pvt->request);
                pvt->request = NULL;
            }
            if(pvt->callback)
            {
                delete pvt->callback;
                pvt->callback = NULL;
            }
            if(pvt->read_resampler)
            {
                switch_resample_destroy(&pvt->read_resampler);
            }
            switch_core_media_bug_flush(bug);
            switch_core_session_reset(pvt->asr_session, SWITCH_TRUE, SWITCH_TRUE);
            break;
        }
        //case SWITCH_ABC_TYPE_READ:
        case SWITCH_ABC_TYPE_READ_REPLACE:
        {
            int16_t *data;
            switch_frame_t *linear_frame;
            uint32_t linear_len = 0;
            char *linear_samples = NULL;
            uint8_t resample_data[SWITCH_RECOMMENDED_BUFFER_SIZE];

            if((linear_frame = switch_core_media_bug_get_read_replace_frame(bug)))
            {
                if(linear_frame->channels != 1)
                {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "nonsupport channels:%d!\n",linear_frame->channels);
                    return SWITCH_FALSE;
                }

                if(pvt->read_resampler)
                {
                    data = (int16_t *)linear_frame->data;
                    switch_resample_process(pvt->read_resampler, data, (int)linear_frame->datalen / 2);
                    linear_len = pvt->read_resampler->to_len * 2;
                    memcpy(resample_data, pvt->read_resampler->to, linear_len);
                    linear_samples = (char *)resample_data;
                }
                else
                {
                    linear_samples = (char *)linear_frame->data;
                    linear_len = linear_frame->datalen;
                }

                if(pvt->request)
                {
                    if(pvt->request->sendAudio(linear_samples, linear_len, false) <= 0)
                    {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, " sendAudio failed. \n");
                        return SWITCH_FALSE;
                    }
                }
            }
            break;
        }
        default:
            break;
    }

    return SWITCH_TRUE;
}

// 加载mod_asr模块时调用的函数
SWITCH_MODULE_LOAD_FUNCTION(mod_asr_load)
{
    switch_application_interface_t *app_interface;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    SWITCH_ADD_APP(app_interface, "start_ali_asr", "aliasr", "aliasr", start_ali_asr_function, "", SAF_NONE);
    SWITCH_ADD_APP(app_interface, "stop_ali_asr", "aliasr", "aliasr", stop_ali_asr_function, "", SAF_NONE);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " asr_load successful...\n");

    return SWITCH_STATUS_SUCCESS;
}

// 卸载mod_asr模块时调用的函数
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_asr_shutdown)
{
    NlsClient::releaseInstance();   // 销毁NlsClient对象实例，只有加载了mod_asr之后才会使用NlsClient对象

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " asr_shutdown successful...\n");

    return SWITCH_STATUS_SUCCESS;
}

// 执行start_ali_asr(APP)命令后调用的函数
SWITCH_STANDARD_APP(start_ali_asr_function)
{
    switch_status_t status;
    switch_asr_t *pvt;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_codec_implementation_t read_impl;
    memset(&read_impl, 0, sizeof(switch_codec_implementation_t));

    char *argv[3] = { 0 };
    int argc;
    char *lbuf = NULL;

    if((pvt = (switch_asr_t *)switch_channel_get_private(channel, ASR_PRIVATE)))
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run asr detection 2 times on the same session!\n");
        return;
    }

    if (!zstr(data) && (lbuf = switch_core_session_strdup(session, data))
        && (argc = switch_separate_string(lbuf, ' ', argv, (sizeof(argv) / sizeof(argv[0])))) >= 3)
    {
        SpeechTranscriberCallback *callback = NULL;

        if (!(pvt = (switch_asr_t *)switch_core_session_alloc(session, sizeof(switch_asr_t))))
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s alloc switch_asr_t failed!\n", switch_channel_get_name(channel));
            return;
        }
        memset(pvt, 0, sizeof(switch_asr_t));

        pvt->asr_session = session;
        pvt->aliAppKey = argv[0];
        pvt->aliAccessKeyID = argv[1];
        pvt->aliAccessKeySecret = argv[2];
        //pvt->aliAppKey = "TYMHcrI1SraL2JaE";
        //pvt->aliAccessKeyID = "LTAIvnwS4DzuqJ1r";
        //pvt->aliAccessKeySecret = "HudYKDlUruRm1iqFk8BJSq0TqZ5Yxe";
        pvt->sample_rate = 8000;

        // 获取token
        std::time_t curTime = std::time(0);
        if (g_token.empty() || g_expireTime - curTime < 10)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " the token will be expired, please generate new token by AccessKey-ID and AccessKey-Secret.\n");
            //if (-1 == generate_token("LTAIvnwS4DzuqJ1r", "HudYKDlUruRm1iqFk8BJSq0TqZ5Yxe", &g_token, &g_expireTime))
            if (-1 == generate_token(pvt->aliAccessKeyID, pvt->aliAccessKeySecret, &g_token, &g_expireTime))
            {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, " Failed to get token.\n");
                return;
            }
        }

        /*
         * 1: 创建并设置回调函数
         */
        callback = new SpeechTranscriberCallback();     // 自定义事件回调参数使用channel
        callback->setOnTranscriptionStarted(onTranscriptionStarted, channel);   // 设置识别启动回调函数
        callback->setOnTranscriptionResultChanged(onTranscriptionResultChanged, channel); // 设置识别结果变化回调函数
        callback->setOnTranscriptionCompleted(onTranscriptionCompleted, channel); // 设置语音转写结束回调函数
        callback->setOnSentenceBegin(onSentenceBegin, channel); // 设置一句话开始回调函数
        callback->setOnSentenceEnd(onSentenceEnd, channel); // 设置一句话结束回调函数
        callback->setOnTaskFailed(onTaskFailed, channel); // 设置异常识别回调函数
        callback->setOnChannelClosed(onChannelClosed, channel); // 设置识别通道关闭回调函数
        pvt->callback = callback;

        int flags = SMBF_READ_REPLACE | SMBF_NO_PAUSE | SMBF_ONE_ONLY;
        status = switch_core_media_bug_add(session, "asr_read", NULL, asr_audio_callback, pvt, 0, flags, &pvt->read_bug);
        if (SWITCH_STATUS_SUCCESS != status)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s switch_core_media_bug_add failed. \n", switch_channel_get_name(channel));
            return;
        }
        switch_channel_set_private(channel, ASR_PRIVATE, pvt);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s start ali Asr recognize!\n", switch_channel_get_name(channel));
    }
    else
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s appKey or AccessKeyID or AccessKeySecret can not be empty\n",
            switch_channel_get_name(channel));
    }
}

// 执行stop_ali_asr(APP)命令后调用的函数
SWITCH_STANDARD_APP(stop_ali_asr_function)
{
    switch_asr_t *pvt;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if((pvt = (switch_asr_t *)switch_channel_get_private(channel, ASR_PRIVATE)))
    {
        if(pvt->request)
        {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "ASR Stop session:%s\n", switch_channel_get_name(channel));
            pvt->request->stop();
            NlsClient::getInstance()->releaseTranscriberRequest(pvt->request);
            pvt->request = NULL;
        }
        if (pvt->callback)
        {
            delete pvt->callback;
            pvt->callback = NULL;
        }

        switch_channel_set_private(channel, ASR_PRIVATE, NULL);
        switch_core_media_bug_remove(session, &pvt->read_bug);
        pvt->read_bug = NULL;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Stop ASR\n", switch_channel_get_name(channel));
    }
}
