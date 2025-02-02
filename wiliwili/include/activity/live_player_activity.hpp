//
// Created by fang on 2022/8/4.
//

#pragma once
#include "view/mpv_core.hpp"
#include "presenter/live_data.hpp"

#include <borealis.hpp>

class VideoView;

class LiveActivity : public brls::Activity, public LiveDataRequest {
public:
    // Declare that the content of this activity is the given XML file
    CONTENT_FROM_XML_RES("activity/video_activity.xml");

    explicit LiveActivity(const bilibili::LiveVideoResult& live);
    explicit LiveActivity(int roomid, const std::string& name = "",
                          const std::string& views = "");

    void setCommonData();

    void setVideoQuality();

    void onContentAvailable() override;

    void onLiveData(const bilibili::LiveRoomPlayInfo& result) override;

    void onError(const std::string& error) override;

    void onNeedPay(const std::string& msg, const std::string& link,
                   const std::string& startTime,
                   const std::string& endTime) override;

    std::vector<std::string> getQualityDescriptionList();
    int getCurrentQualityIndex();

    void retryRequestData();

    ~LiveActivity() override;

private:
    BRLS_BIND(VideoView, video, "fullscreen/video");

    // 暂停的延时函数 handle
    size_t toggleDelayIter = 0;
    // 遇到错误重试的延时函数 handle
    size_t errorDelayIter = 0;

    bilibili::LiveVideoResult liveData;

    //更新timeLabel
    MPVEvent::Subscription tl_event_id;
    //视频清晰度
    MPVCustomEvent::Subscription event_id;
};