# mod_asr
fs的asr模块，使用阿里ASR的SDK

# 阿里asr依赖库安装
执行`./installEnv`即可

# 模块安装
g++ -shared -fPIC -o mod_asr.so mod_asr.cpp -pthread -I ./NlsSdkCpp2.0/include -I /usr/local/freeswitch/include/freeswitch -I /root/freeswitch/src/include -L ./NlsSdkCpp2.0/lib/linux -L /usr/local/freeswitch/lib -ldl -lopus -lfreeswitch -lnlsCppSdk -lnlsCommonSdk -luuid -ljsoncpp

# 模块加载
`cp mod_asr.so /usr/local/freeswitch/mod/`

# 模块使用 #
1. 启动ASR识别,执行`start_ali_asr AppKey AccessKeyId AccessKeySecret`，结束vad检测执行
2. 关闭ASR识别,执行`stop_ali_asr`

# 注
1. 使用的是阿里的ASR实时语音识别speechTranscriber，该SDK中自带了vad，可通过onSentenceEnd回调来获取每句话的识别结果