using Avalonia.Threading;
using Mesen.Config;
using Mesen.Config.Shortcuts;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.Utilities;
using ReactiveUI;
using ReactiveUI.Fody.Helpers;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reactive.Linq;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Mesen.ViewModels
{
	public class SmsHdPackBuilderViewModel : DisposableViewModel
	{
		[Reactive] public string SaveFolder { get; set; }
		[Reactive] public bool IsRecording { get; set; }
		[Reactive] public bool IsOpenFolderEnabled { get; set; }
		[Reactive] public SmsHdPackBuilderConfig Config { get; set; }
		[Reactive] public FilterInfo? SelectedFilter { get; set; }
		[Reactive] public bool ScaleLocked { get; set; }
		[Reactive] public bool IsFilterEnabled { get; set; }
		
		[Reactive] public FilterInfo[] Filters { get; private set; } = Array.Empty<FilterInfo>();

		private FilterInfo[] _allFilters = Array.Empty<FilterInfo>();

		public SmsHdPackBuilderViewModel()
		{
			Config = ConfigManager.Config.SmsHdPackBuilder;
			SaveFolder = Path.Join(ConfigManager.HdPackFolder, EmuApi.GetRomInfo().GetRomName());
			ScaleLocked = false;
			IsFilterEnabled = true;
			// Sync UI with core recording state so closing/reopening window keeps recording active until user presses Stop
			IsRecording = EmuApi.SmsIsHdPackRecording();

			UpdateFilterDropdown();

			SelectedFilter = Filters.Where(x => x.FilterType == Config.FilterType && x.Scale == Config.Scale).FirstOrDefault() ?? Filters[0];

			// If an existing hires.txt is present and indicates a scale, lock the scale to that value.
			TryLockScaleFromExistingManifest();
			
			AddDisposable(this.WhenAnyValue(x => x.SelectedFilter).Subscribe(filter => {
				if(filter != null) {
					Config.FilterType = filter.FilterType;
					Config.Scale = filter.Scale;
				}
			}));

			AddDisposable(this.WhenAnyValue(x => x.IsRecording).Subscribe(recording => {
				IsOpenFolderEnabled = !recording && Directory.Exists(SaveFolder);
				IsFilterEnabled = !recording && !ScaleLocked;
			}));

			// Note: State change notifications will be handled by the emulator core
		}



		private void UpdateFilterDropdown()
		{
			FilterInfo? selectedFilter = SelectedFilter;

			List<FilterInfo> filters = new();
			for(uint scale = 1; scale <= 10; scale++) {
				filters.Add(new FilterInfo() { Name = scale + "x (Prescale)", FilterType = ScaleFilterType.Prescale, Scale = scale });
			}

			// Add other scale filters for scales 2-6
			for(uint scale = 2; scale <= 6; scale++) {
				filters.Add(new FilterInfo() { Name = scale + "x (xBRZ)", FilterType = ScaleFilterType.xBRZ, Scale = scale });
				filters.Add(new FilterInfo() { Name = scale + "x (HQX)", FilterType = ScaleFilterType.HQX, Scale = scale });
			}

			// Add 2x and 3x specific filters
			filters.Add(new FilterInfo() { Name = "2x (Scale2x)", FilterType = ScaleFilterType.Scale2x, Scale = 2 });
			filters.Add(new FilterInfo() { Name = "2x (2xSai)", FilterType = ScaleFilterType._2xSai, Scale = 2 });
			filters.Add(new FilterInfo() { Name = "2x (Super 2xSai)", FilterType = ScaleFilterType.Super2xSai, Scale = 2 });
			filters.Add(new FilterInfo() { Name = "2x (Super Eagle)", FilterType = ScaleFilterType.SuperEagle, Scale = 2 });

			_allFilters = filters.ToArray();

			// Filter out unavailable filters based on current console state
			if(selectedFilter != null) {
				foreach(FilterInfo filter in _allFilters) {
					if(filter.FilterType == selectedFilter.FilterType && filter.Scale == selectedFilter.Scale) {
						selectedFilter = filter;
						break;
					}
				}
			}

			Filters = _allFilters;
			SelectedFilter = selectedFilter;
		}

		private void TryLockScaleFromExistingManifest()
		{
			try {
				string manifest = Path.Combine(SaveFolder, "hires.txt");
				if(!File.Exists(manifest)) {
					return;
				}

				int? scale = ParseScaleFromManifest(manifest);
				if(scale.HasValue && scale.Value >= 1 && scale.Value <= 10) {
					ScaleLocked = true;
					IsFilterEnabled = !IsRecording && !ScaleLocked;
					// Update config and selection to the locked scale
					Config.Scale = (uint)scale.Value;
					UpdateFilterDropdown();
					SelectedFilter = Filters.Where(x => x.Scale == Config.Scale && x.FilterType == Config.FilterType).FirstOrDefault()
						?? Filters.Where(x => x.Scale == Config.Scale).FirstOrDefault()
						?? Filters[0];
				}
			} catch { /* ignore parse errors */ }
		}

		private static int? ParseScaleFromManifest(string manifestPath)
		{
			foreach(string line in File.ReadLines(manifestPath)) {
				string l = line.Trim();
				if(l.StartsWith("<scale>", StringComparison.OrdinalIgnoreCase)) {
					// formats supported: "<scale>4" or "<scale>4</scale>"
					string rest = l.Substring("<scale>".Length).Trim();
					int end = rest.IndexOf('<');
					if(end >= 0) rest = rest.Substring(0, end);
					if(int.TryParse(rest.Trim(), out int s)) {
						return s;
					}
				}
			}
			return null;
		}

		public void StartRecording()
		{
			if(IsRecording) {
				return;
			}

			IsRecording = true;

			Task.Run(() => {
				HdPackBuilderOptions options = Config.ToInterop(SaveFolder);

				IntPtr optionsPtr = Marshal.AllocHGlobal(Marshal.SizeOf(options));
				try {
					Marshal.StructureToPtr(options, optionsPtr, false);
					EmuApi.ExecuteShortcut(new ExecuteShortcutParams() {
						Shortcut = EmulatorShortcut.StartRecordSmsHdPack,
						ParamPtr = optionsPtr
					});
				} finally {
					Marshal.FreeHGlobal(optionsPtr);
				}
			});
		}

		public void StopRecording()
		{
			if(!IsRecording) {
				return;
			}

			IsRecording = false;

			Task.Run(() => {
				EmuApi.ExecuteShortcut(new ExecuteShortcutParams() { Shortcut = EmulatorShortcut.StopRecordSmsHdPack });

				Dispatcher.UIThread.Post(() => {
					IsOpenFolderEnabled = true;
					UpdateFilterDropdown();
				});
			});
		}

		public void OpenFolder()
		{
			if(Directory.Exists(SaveFolder)) {
				System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo() {
					FileName = SaveFolder + Path.DirectorySeparatorChar,
					UseShellExecute = true,
					Verb = "open"
				});
			}
		}

		public class FilterInfo
		{
			public string Name { get; set; } = "";
			public ScaleFilterType FilterType { get; set; }
			public UInt32 Scale { get; set; }

			public override string ToString() => Name;
		}
	}
}
