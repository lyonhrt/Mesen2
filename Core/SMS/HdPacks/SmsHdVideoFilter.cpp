#include "pch.h"
#include "SMS/HdPacks/SmsHdVideoFilter.h"
#include "SMS/HdPacks/SmsHdPack.h"
#include "SMS/HdPacks/SmsHdPackApi.h"
#include "SMS/SmsConsole.h"
#include "Shared/Emulator.h"
#include "SMS/HdPacks/HdDataSms.h"

SmsHdVideoFilter::SmsHdVideoFilter(SmsConsole* console, Emulator* emu, HdPackDataSms* hdData) : BaseVideoFilter(emu)
{
	_console = console;
	_hdData = hdData;
	
	// Get scale from API if HD pack is loaded via API, otherwise from hdData
	// Like NES, we support scales 1-10
	uint32_t scale = 1;
	if(SmsHdPackApi::IsHdPackLoaded()) {
		scale = SmsHdPackApi::GetHdPackScale();
	} else if(hdData) {
		scale = hdData->Scale;
	}
	if(scale < 1) scale = 1;
	if(scale > 10) scale = 10;
	
	switch(scale) {
		case 1: _smsHdPack.reset(new SmsHdPack<1>(console, emu->GetSettings(), hdData)); break;
		case 2: _smsHdPack.reset(new SmsHdPack<2>(console, emu->GetSettings(), hdData)); break;
		case 3: _smsHdPack.reset(new SmsHdPack<3>(console, emu->GetSettings(), hdData)); break;
		case 4: _smsHdPack.reset(new SmsHdPack<4>(console, emu->GetSettings(), hdData)); break;
		case 5: _smsHdPack.reset(new SmsHdPack<5>(console, emu->GetSettings(), hdData)); break;
		case 6: _smsHdPack.reset(new SmsHdPack<6>(console, emu->GetSettings(), hdData)); break;
		case 7: _smsHdPack.reset(new SmsHdPack<7>(console, emu->GetSettings(), hdData)); break;
		case 8: _smsHdPack.reset(new SmsHdPack<8>(console, emu->GetSettings(), hdData)); break;
		case 9: _smsHdPack.reset(new SmsHdPack<9>(console, emu->GetSettings(), hdData)); break;
		case 10: _smsHdPack.reset(new SmsHdPack<10>(console, emu->GetSettings(), hdData)); break;
	}
}

void SmsHdVideoFilter::ApplyFilter(uint16_t* vdpOutputBuffer)
{
	if(!_smsHdPack) {
		return;
	}

	OverscanDimensions overscan = GetOverscan();
	
	// Pass nullptr for hdScreenInfo - SmsHdPack::Process reads directly from VDP's HdPixelInfo
	_smsHdPack->Process(nullptr, GetOutputBuffer(), overscan);
}

FrameInfo SmsHdVideoFilter::GetFrameInfo()
{
	OverscanDimensions overscan = GetOverscan();
	uint32_t hdScale = _smsHdPack ? _smsHdPack->GetScale() : 1;
	
	return {
		(SmsScreenWidth - overscan.Left - overscan.Right) * hdScale,
		(SmsScreenHeight - overscan.Top - overscan.Bottom) * hdScale
	};
}

OverscanDimensions SmsHdVideoFilter::GetOverscan()
{
	if(_hdData && _hdData->HasOverscanConfig) {
		return _hdData->Overscan;
	}
	return BaseVideoFilter::GetOverscan();
}