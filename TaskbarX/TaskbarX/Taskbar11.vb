Option Strict On

Imports System.IO
Imports System.Runtime.InteropServices
Imports System.Text
Imports System.Threading
Imports Microsoft.Win32

'Windows 11 support.
'The Windows 11 taskbar is drawn with XAML inside explorer.exe, so there is no MSTaskListWClass to move.
'Instead TaskbarX loads TaskbarX11Hook.dll into explorer.exe, which moves the XAML buttons itself
'(Start to the left, app icons centered, Widgets hidden). Its settings are passed through the registry.
Public Class Taskbar11

    Private Const SettingsKey As String = "Software\TaskbarX\Win11"
    Private Const HookDll As String = "TaskbarX11Hook.dll"
    Private Const WH_CALLWNDPROC As Integer = 4

    <DllImport("kernel32.dll", CharSet:=CharSet.Unicode, SetLastError:=True)>
    Private Shared Function LoadLibrary(ByVal lpFileName As String) As IntPtr
    End Function

    <DllImport("kernel32.dll", CharSet:=CharSet.Ansi, ExactSpelling:=True)>
    Private Shared Function GetProcAddress(ByVal hModule As IntPtr, ByVal procName As String) As IntPtr
    End Function

    <DllImport("user32.dll", SetLastError:=True)>
    Private Shared Function SetWindowsHookEx(ByVal idHook As Integer, ByVal lpfn As IntPtr, ByVal hMod As IntPtr, ByVal dwThreadId As UInteger) As IntPtr
    End Function

    <DllImport("user32.dll")>
    Private Shared Function UnhookWindowsHookEx(ByVal hhk As IntPtr) As Boolean
    End Function

    <DllImport("user32.dll", CharSet:=CharSet.Unicode)>
    Private Shared Function RegisterWindowMessage(ByVal lpString As String) As UInteger
    End Function

    <DllImport("user32.dll")>
    Private Shared Function SendMessageTimeout(ByVal hWnd As IntPtr, ByVal msg As UInteger, ByVal wParam As IntPtr, ByVal lParam As IntPtr, ByVal flags As UInteger, ByVal timeout As UInteger, ByRef result As IntPtr) As IntPtr
    End Function

    <DllImport("user32.dll")>
    Private Shared Function GetWindowThreadProcessId(ByVal hWnd As IntPtr, ByRef lpdwProcessId As UInteger) As UInteger
    End Function

    <DllImport("user32.dll")>
    Private Shared Function EnumChildWindows(ByVal hWndParent As IntPtr, ByVal lpEnumFunc As EnumChildProc, ByVal lParam As IntPtr) As Boolean
    End Function

    Private Delegate Function EnumChildProc(ByVal hwnd As IntPtr, ByVal lParam As IntPtr) As Boolean

    Public Shared Function WindowsBuild() As Integer
        Try
            Using key = Registry.LocalMachine.OpenSubKey("SOFTWARE\Microsoft\Windows NT\CurrentVersion")
                Return CInt(key.GetValue("CurrentBuildNumber", "0"))
            End Using
        Catch
            Return 0
        End Try
    End Function

    'True when Shell_TrayWnd is the Windows 11 XAML taskbar (and not e.g. the classic taskbar restored by ExplorerPatcher).
    Public Shared Function IsXamlTaskbar(ByVal Shell_TrayWnd As IntPtr) As Boolean
        If Shell_TrayWnd = IntPtr.Zero OrElse WindowsBuild() < 22000 Then
            Return False
        End If

        Dim found As Boolean = False
        EnumChildWindows(Shell_TrayWnd, Function(hwnd, lParam)
                                            Dim sClassName As New StringBuilder("", 256)
                                            Win32.GetClassName(hwnd, sClassName, 256)
                                            If sClassName.ToString = "Windows.UI.Composition.DesktopWindowContentBridge" Then
                                                found = True
                                                Return False
                                            End If
                                            Return True
                                        End Function, IntPtr.Zero)
        Return found
    End Function

    Public Shared Sub WriteSettings()
        Using key = Registry.CurrentUser.CreateSubKey(SettingsKey)
            key.SetValue("Active", 1, RegistryValueKind.DWord)
            key.SetValue("StartOnLeft", Settings.Win11StartOnLeft, RegistryValueKind.DWord)
            key.SetValue("KeepButtonsLeft", Settings.Win11KeepButtonsLeft, RegistryValueKind.DWord)
            key.SetValue("HideWidgets", Settings.Win11HideWidgets, RegistryValueKind.DWord)
            key.SetValue("StartPadding", Settings.Win11StartPadding, RegistryValueKind.DWord)
            key.SetValue("CenterIcons", If(Settings.DontCenterTaskbar = 1, 0, 1), RegistryValueKind.DWord)
            key.SetValue("CenterInBetween", Settings.CenterInBetween, RegistryValueKind.DWord)
            key.SetValue("Offset", Settings.PrimaryTaskbarOffset, RegistryValueKind.DWord)
            key.SetValue("TaskbarStyle", Settings.TaskbarStyle, RegistryValueKind.DWord)
            key.SetValue("AnimationSpeed", Settings.AnimationSpeed, RegistryValueKind.DWord)
            key.SetValue("AnimationStyle", Settings.AnimationStyle, RegistryValueKind.String)
            key.SetValue("Debug", Settings.Win11Debug, RegistryValueKind.DWord)
        End Using
    End Sub

    'Tells a hook that is already loaded to re-read its settings.
    Private Shared Sub SignalReload()
        Dim reload As EventWaitHandle = Nothing
        If EventWaitHandle.TryOpenExisting("Local\TaskbarX11Hook_Reload", reload) Then
            reload.Set()
            reload.Dispose()
        End If
    End Sub

    Private Shared Function IsHookLoaded(ByVal explorerPid As UInteger) As Boolean
        Dim active As Mutex = Nothing
        If Mutex.TryOpenExisting("Local\TaskbarX11Hook_Active_" & explorerPid, active) Then
            active.Dispose()
            Return True
        End If
        Return False
    End Function

    Private Shared Function Inject(ByVal Shell_TrayWnd As IntPtr, ByVal threadId As UInteger, ByVal explorerPid As UInteger) As Boolean
        If IsHookLoaded(explorerPid) Then
            SignalReload()
            Return True
        End If

        If Not Environment.Is64BitProcess AndAlso Environment.Is64BitOperatingSystem Then
            Console.WriteLine("Windows 11 support needs the 64-bit build of TaskbarX.")
            Return False
        End If

        Dim dllPath As String = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, HookDll)
        If Not File.Exists(dllPath) Then
            Console.WriteLine(HookDll & " is missing next to TaskbarX.exe.")
            Return False
        End If

        Dim hModule As IntPtr = LoadLibrary(dllPath)
        Dim hookProc As IntPtr = GetProcAddress(hModule, "TaskbarX11HookProc")
        If hookProc = IntPtr.Zero Then
            Console.WriteLine("Could not load " & dllPath)
            Return False
        End If

        'The hook gets the DLL into explorer.exe; the DLL then attaches itself to the taskbar's XAML and pins itself,
        'so the hook can be removed again right after.
        Dim hook As IntPtr = SetWindowsHookEx(WH_CALLWNDPROC, hookProc, hModule, threadId)
        If hook = IntPtr.Zero Then
            Console.WriteLine("SetWindowsHookEx failed: " & Marshal.GetLastWin32Error())
            Return False
        End If

        Try
            Dim result As IntPtr
            SendMessageTimeout(Shell_TrayWnd, RegisterWindowMessage("TaskbarX11Hook_Inject"), IntPtr.Zero, IntPtr.Zero, 2, 5000, result)

            For i = 1 To 60
                If IsHookLoaded(explorerPid) Then
                    Console.WriteLine("TaskbarX11Hook loaded into explorer.exe (" & explorerPid & ")")
                    Return True
                End If
                Thread.Sleep(250)
            Next
            Return False
        Finally
            UnhookWindowsHookEx(hook)
        End Try
    End Function

    'Keeps the hook loaded for as long as TaskbarX runs, also after explorer.exe restarts.
    Public Shared Sub Run()
        Dim injectedPid As UInteger = 0
        Do
            Try
                Dim Shell_TrayWnd = Win32.FindWindowByClass("Shell_TrayWnd", CType(0, IntPtr))
                If Not Shell_TrayWnd = IntPtr.Zero Then
                    Dim explorerPid As UInteger
                    Dim threadId = GetWindowThreadProcessId(Shell_TrayWnd, explorerPid)
                    If Not explorerPid = injectedPid AndAlso Inject(Shell_TrayWnd, threadId, explorerPid) Then
                        injectedPid = explorerPid
                    End If
                End If
            Catch ex As Exception
                Console.WriteLine(ex.Message)
            End Try
            Thread.Sleep(2000)
        Loop
    End Sub

    'True when the hook is loaded in the running explorer.exe and currently moving the taskbar's buttons.
    Public Shared Function IsOn() As Boolean
        Try
            Using key = Registry.CurrentUser.OpenSubKey(SettingsKey)
                If key Is Nothing OrElse Not CInt(key.GetValue("Active", 0)) = 1 Then
                    Return False
                End If
            End Using
            Dim Shell_TrayWnd = Win32.FindWindowByClass("Shell_TrayWnd", CType(0, IntPtr))
            Dim explorerPid As UInteger
            GetWindowThreadProcessId(Shell_TrayWnd, explorerPid)
            Return explorerPid <> 0 AndAlso IsHookLoaded(explorerPid)
        Catch
            Return False
        End Try
    End Function

    'Puts the taskbar back the way Windows draws it. The hook stays loaded but does nothing until TaskbarX starts again.
    Public Shared Sub [Stop]()
        Try
            Using key = Registry.CurrentUser.OpenSubKey(SettingsKey, True)
                If key Is Nothing Then
                    Return 'never ran on Windows 11
                End If
                key.SetValue("Active", 0, RegistryValueKind.DWord)
            End Using
            SignalReload()
        Catch ex As Exception
            Console.WriteLine(ex.Message)
        End Try
    End Sub

End Class
