#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"

class Emulator;
class SmsConsole;
struct HdPackDataSms;

class SmsHdVideoFilter : public BaseVideoFilter
{
private:
	HdPackDataSms* _hdData;

public:
	SmsHdVideoFilter(SmsConsole* console, Emulator* emu, HdPackDataSms* hdData);
	virtual ~SmsHdVideoFilter() = default;

	void ApplyFilter(uint16_t *vdpOutputBuffer) override;
	FrameInfo GetFrameInfo() override;
	OverscanDimensions GetOverscan() override;
};