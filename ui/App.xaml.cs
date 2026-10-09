using System;
using System.IO;
using System.Windows;
using System.Windows.Threading;

namespace NRLiveUI;

public partial class App : Application
{
    private static string LogPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "NRLive",
        "launcher-startup.log");

    protected override void OnStartup(StartupEventArgs e)
    {
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnUnhandledException;

        try
        {
            base.OnStartup(e);
            var window = new MainWindow();
            MainWindow = window;
            window.Show();
        }
        catch (Exception ex)
        {
            LogException("Launcher startup failed", ex);
            MessageBox.Show(
                $"NRLive UI could not start.\n\nDetails were saved to:\n{LogPath}\n\n{ex.Message}",
                "NRLive startup error",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Shutdown(1);
        }
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        LogException("Unhandled WPF dispatcher exception", e.Exception);
        MessageBox.Show(
            $"NRLive encountered a UI error. Details were saved to:\n{LogPath}\n\n{e.Exception.Message}",
            "NRLive UI error",
            MessageBoxButton.OK,
            MessageBoxImage.Error);
        e.Handled = true;
    }

    private static void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
    {
        if (e.ExceptionObject is Exception ex)
            LogException("Unhandled application exception", ex);
    }

    private static void LogException(string heading, Exception ex)
    {
        try
        {
            string? directory = Path.GetDirectoryName(LogPath);
            if (!string.IsNullOrEmpty(directory))
                Directory.CreateDirectory(directory);
            File.AppendAllText(LogPath,
                $"[{DateTimeOffset.Now:O}] {heading}{Environment.NewLine}{ex}{Environment.NewLine}{Environment.NewLine}");
        }
        catch
        {
            // Startup diagnostics must never trigger another exception.
        }
    }
}
