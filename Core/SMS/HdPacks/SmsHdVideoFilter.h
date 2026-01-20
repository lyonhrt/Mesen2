#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "SMS/HdPacks/SmsHdPack.h"

class Emulator;
class SmsConsole;
struct HdPackDataSms;
struct HdScreenInfoSms;

class SmsHdVideoFilter : public BaseVideoFilter
{
private:
	SmsConsole* _console = nullptr;
	HdPackDataSms* _hdData = nullptr;
	unique_ptr<BaseSmsHdPack> _smsHdPack = nullptr;

public:
	SmsHdVideoFilter(SmsConsole* console, Emulator* emu, HdPackDataSms* hdData);
	virtual ~SmsHdVideoFilter() = default;

	void ApplyFilter(uint16_t *vdpOutputBuffer) override;
	FrameInfo GetFrameInfo() override;
	OverscanDimensions GetOverscan() override;
	
	BaseSmsHdPack* GetHdPack() { return _smsHdPack.get(); }
};