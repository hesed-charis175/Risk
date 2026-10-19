const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function activate(context) {
  const cfg = vscode.workspace.getConfiguration("risk");
  const command = cfg.get("serverPath") || "risk-lsp";
  client = new LanguageClient(
    "risk",
    "Risk Language Server",
    { command, args: [] },
    {
      documentSelector: [{ scheme: "file", language: "risk" }],
      initializationOptions: { stdlibPath: cfg.get("stdlibPath") || "" },
    }
  );
  client.start();
  context.subscriptions.push({ dispose: () => client && client.stop() });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
