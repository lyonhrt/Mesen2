using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Mesen.ViewModels;

namespace Mesen.Views
{
	public partial class SmsHdPackBuilderWindow : MesenWindow
	{
		public SmsHdPackBuilderWindow()
		{
			AvaloniaXamlLoader.Load(this);
			DataContext = new SmsHdPackBuilderViewModel();
		}

		private void StartRecording_Click(object? sender, RoutedEventArgs e)
		{
			if(DataContext is SmsHdPackBuilderViewModel vm) {
				vm.StartRecording();
			}
		}

		private void StopRecording_Click(object? sender, RoutedEventArgs e)
		{
			if(DataContext is SmsHdPackBuilderViewModel vm) {
				vm.StopRecording();
			}
		}

		private void OpenFolder_Click(object? sender, RoutedEventArgs e)
		{
			if(DataContext is SmsHdPackBuilderViewModel vm) {
				vm.OpenFolder();
			}
		}

		private void Close_Click(object? sender, RoutedEventArgs e)
		{
			Close();
		}
	}
}
