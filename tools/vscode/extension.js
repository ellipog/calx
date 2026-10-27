// The Calx extension. Highlighting comes from the grammars in syntaxes/. This file shows the
// compiler's errors as you type, by running `calx check --json --stdin` on the open file, and
// runs files through `calx run`. The language server (P9.2) will take this over.
'use strict';

const vscode = require('vscode');
const childProcess = require('child_process');
const os = require('os');
const path = require('path');
const { parseDiagnostics, samePath, diagnosticsFor, findCompiler } = require('./compiler');

// `calx check` takes milliseconds, so a short pause after typing is enough to skip keystrokes.
const CHECK_DELAY_MS = 150;
const CHECK_TIMEOUT_MS = 10000;

let collection;
const timers = new Map();   // document URI to its pending check
const running = new Map();  // document URI to the compiler checking it
let warned = null;          // the compiler we last said couldn't run, so it's said once

function activate(context) {
    collection = vscode.languages.createDiagnosticCollection('calx');
    context.subscriptions.push(
        collection,
        vscode.workspace.onDidOpenTextDocument((doc) => schedule(doc, 0)),
        vscode.workspace.onDidChangeTextDocument((e) => {
            if (setting(e.document, 'checkOnType')) schedule(e.document, CHECK_DELAY_MS);
        }),
        vscode.workspace.onDidSaveTextDocument((doc) => schedule(doc, 0)),
        vscode.workspace.onDidCloseTextDocument(forget),
        vscode.workspace.onDidChangeConfiguration((e) => {
            if (!e.affectsConfiguration('calx')) return;
            warned = null;
            for (const doc of vscode.workspace.textDocuments) schedule(doc, 0);
        }),
        vscode.commands.registerCommand('calx.runFile', runFile),
        vscode.tasks.registerTaskProvider('calx', { provideTasks: () => [], resolveTask }),
        { dispose: stopAll },
    );
    for (const doc of vscode.workspace.textDocuments) schedule(doc, 0);
}

function deactivate() {
    stopAll();
}

function setting(doc, name) {
    return vscode.workspace.getConfiguration('calx', doc.uri).get(name);
}

function filePath(doc) {
    return doc.uri.scheme === 'file' ? doc.uri.fsPath : doc.fileName;
}

function compilerFor(doc) {
    let configured = setting(doc, 'compilerPath') || '';
    if (configured.startsWith('~')) configured = path.join(os.homedir(), configured.slice(1));
    if (configured && !path.isAbsolute(configured) && configured.includes(path.sep)) {
        const folder = vscode.workspace.getWorkspaceFolder(doc.uri) ?? vscode.workspace.workspaceFolders?.[0];
        if (folder) configured = path.join(folder.uri.fsPath, configured);
    }
    const starts = [];
    if (doc.uri.scheme === 'file') starts.push(path.dirname(doc.uri.fsPath));
    for (const folder of vscode.workspace.workspaceFolders ?? []) starts.push(folder.uri.fsPath);
    return findCompiler({ configured, starts });
}

// --- checking -----------------------------------------------------------------------------------

function schedule(doc, delay) {
    if (doc.languageId !== 'calx') return;
    const key = doc.uri.toString();
    clearTimeout(timers.get(key));
    timers.set(key, setTimeout(() => {
        timers.delete(key);
        check(doc);
    }, delay));
}

function forget(doc) {
    const key = doc.uri.toString();
    clearTimeout(timers.get(key));
    timers.delete(key);
    running.get(key)?.kill();
    running.delete(key);
    collection.delete(doc.uri);
}

function stopAll() {
    for (const t of timers.values()) clearTimeout(t);
    for (const child of running.values()) child.kill();
    timers.clear();
    running.clear();
}

function check(doc) {
    if (doc.isClosed) return;
    const key = doc.uri.toString();
    running.get(key)?.kill();
    const file = filePath(doc);
    const compiler = compilerFor(doc);
    const version = doc.version;
    const cwd = doc.uri.scheme === 'file' ? path.dirname(file) : undefined;
    let child;
    try {
        child = childProcess.spawn(compiler, ['check', '--json', '--stdin', file], { cwd, windowsHide: true });
    } catch (err) {
        cantRun(compiler, err.message);
        return;
    }
    running.set(key, child);
    let stdout = '';
    let stderr = '';
    const timeout = setTimeout(() => child.kill(), CHECK_TIMEOUT_MS);
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (s) => { stdout += s; });
    child.stderr.on('data', (s) => { stderr += s; });
    child.stdin.on('error', () => {});  // it may exit before reading everything
    child.on('error', (err) => {
        clearTimeout(timeout);
        if (running.get(key) === child) running.delete(key);
        cantRun(compiler, err.code === 'ENOENT' ? 'it wasn\'t found' : err.message);
    });
    child.on('close', (code, signal) => {
        clearTimeout(timeout);
        if (running.get(key) !== child) return;  // a newer check replaced this one
        running.delete(key);
        if (signal || doc.isClosed || doc.version !== version) return;
        if (code === 2) {
            // A command-line error: most likely a compiler from before `--json`.
            cantRun(compiler, stderr.trim() || 'it rejected the command line; rebuild it');
            return;
        }
        publish(doc, file, parseDiagnostics(stdout));
    });
    child.stdin.end(doc.getText(), 'utf8');
}

function cantRun(compiler, why) {
    if (warned === compiler) return;
    warned = compiler;
    const open = 'Open Settings';
    vscode.window
        .showWarningMessage(`Calx: can't run the compiler at ${compiler}: ${why}. Set calx.compilerPath to show errors.`, open)
        .then((choice) => {
            if (choice === open) vscode.commands.executeCommand('workbench.action.openSettings', 'calx.compilerPath');
        });
}

const SEVERITY = {
    error: vscode.DiagnosticSeverity.Error,
    warning: vscode.DiagnosticSeverity.Warning,
    note: vscode.DiagnosticSeverity.Information,
};

function toRange(r) {
    return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function publish(doc, file, diags) {
    const out = [];
    for (const d of diagnosticsFor(diags, file)) {
        const diag = new vscode.Diagnostic(toRange(d.range), d.message, SEVERITY[d.severity] ?? vscode.DiagnosticSeverity.Error);
        diag.source = 'calx';
        diag.relatedInformation = (d.notes ?? [])
            .filter((n) => n.file)
            .map((n) => {
                const uri = samePath(n.file, file) ? doc.uri : vscode.Uri.file(path.resolve(path.dirname(file), n.file));
                return new vscode.DiagnosticRelatedInformation(new vscode.Location(uri, toRange(n.range)), n.message);
            });
        out.push(diag);
    }
    collection.set(doc.uri, out);
}

// --- running ------------------------------------------------------------------------------------

async function runFile(uri) {
    const doc = uri instanceof vscode.Uri
        ? await vscode.workspace.openTextDocument(uri)
        : vscode.window.activeTextEditor?.document;
    if (!doc || doc.languageId !== 'calx') {
        vscode.window.showInformationMessage('Calx: open a .calx file to run it.');
        return;
    }
    if ((doc.isDirty || doc.isUntitled) && !(await doc.save())) return;
    const task = makeTask({ type: 'calx', command: 'run', file: doc.uri.fsPath }, doc.uri.fsPath, doc);
    await vscode.tasks.executeTask(task);
}

// Runs `calx <command> <file>`, for the run command and for `"type": "calx"` tasks in tasks.json.
function makeTask(definition, file, doc, scope) {
    const args = [definition.command, file];
    if (definition.release) args.push('--release');
    const folder = scope ?? vscode.workspace.getWorkspaceFolder(doc.uri) ?? vscode.TaskScope.Workspace;
    const execution = new vscode.ProcessExecution(compilerFor(doc), args, { cwd: path.dirname(file) });
    const task = new vscode.Task(definition, folder, `${definition.command} ${path.basename(file)}`, 'calx', execution, []);
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated, clear: true };
    return task;
}

async function resolveTask(task) {
    const definition = task.definition;
    if (!definition.file || !definition.command) return undefined;
    const folder = typeof task.scope === 'object' ? task.scope : undefined;
    const file = path.isAbsolute(definition.file) || !folder ? definition.file : path.join(folder.uri.fsPath, definition.file);
    const doc = await vscode.workspace.openTextDocument(file);
    return makeTask(definition, file, doc, task.scope);
}

module.exports = { activate, deactivate };
