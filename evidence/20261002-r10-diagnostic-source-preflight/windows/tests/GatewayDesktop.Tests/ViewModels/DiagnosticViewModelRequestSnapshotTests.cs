using System.Reflection;
using GatewayDesktop.Core.Entities;
using GatewayDesktop.Core.Interfaces;
using GatewayDesktop.UI.Infrastructure;
using GatewayDesktop.UI.ViewModels;
using Moq;
using UiAppContext = GatewayDesktop.UI.Infrastructure.AppContext;

namespace GatewayDesktop.Tests.ViewModels;

public sealed class DiagnosticViewModelRequestSnapshotTests
{
    [Fact]
    public async Task Deferred_result_keeps_executed_command_target_and_parameters_after_selection_changes()
    {
        var deferred = new TaskCompletionSource<CommandResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        var commands = new List<DiagnosticCommand>
        {
            new() { Name = "Service A", Command = "systemctl_status" },
            new() { Name = "Memory B", Command = "free_m" },
            new() { Name = "Journal", Command = "journal_tail" }
        };
        DiagnosticCommand? sentCommand = null;
        string? sentService = null;
        var service = new Mock<IDiagnosticService>();
        service.Setup(x => x.GetPredefinedCommandsAsync(It.IsAny<CancellationToken>())).ReturnsAsync(commands);
        service.Setup(x => x.ExecuteAsync(It.IsAny<DiagnosticCommand>(), It.IsAny<string>(), It.IsAny<int>(),
                It.IsAny<CancellationToken>()))
            .Returns((DiagnosticCommand command, string unit, int _, CancellationToken _) =>
            {
                sentCommand = command;
                sentService = unit;
                return deferred.Task;
            });
        var context = new UiAppContext(Mock.Of<IConnectionService>(), new ConfigWorkspace())
        { MachineCode = "GW_A" };
        GatewayDesktop.UI.Infrastructure.ActionRecordedEventArgs? action = null;
        context.ActionRecorded += (_, recorded) => action = recorded;
        var vm = new DiagnosticViewModel(service.Object, context);
        vm.SelectedCommand = commands[0];
        vm.ServiceName = "gateway-services.service";
        var running = vm.ExecuteCommand.ExecuteAsync(null);
        Assert.Equal("systemctl_status", sentCommand?.Command);
        Assert.Equal("gateway-services.service", sentService);

        vm.SelectedCommand = commands[1];
        vm.ServiceName = "other.service";
        vm.TailLines = 300;
        context.MachineCode = "GW_B";
        deferred.SetResult(new CommandResult
        {
            Success = true, Output = "Active: active (running)", ExitCode = 0,
            CmdId = "DIAG_FIXED", MachineCode = "GW_A", Reason = "exit", CleanupConfirmed = true
        });
        await running;

        Assert.Contains("Service A", vm.ResultSummary);
        Assert.DoesNotContain("Memory B", vm.ResultSummary);
        Assert.Contains("GW_A/systemctl_status/DIAG_FIXED", action?.Target);
        var export = (string)typeof(DiagnosticViewModel)
            .GetMethod("BuildExportText", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(vm, null)!;
        Assert.Contains("Command: Service A (systemctl_status)", export);
        Assert.Contains("Service: gateway-services.service", export);
        Assert.Contains("MachineCode: GW_A", export);
        Assert.Contains("CmdId: DIAG_FIXED", export);
        Assert.DoesNotContain("other.service", export);

        vm.SelectedCommand = commands[2];
        Assert.False(vm.NeedsService);
        Assert.True(vm.NeedsTailLines);
    }
}
