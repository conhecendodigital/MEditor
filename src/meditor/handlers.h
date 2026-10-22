/*
    SPDX-FileCopyrightText: 2026 MEditor Project
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "undohelper.hpp"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

class MEditorApiServer;

namespace MEditor {

/**
 * Handlers da API JSON-RPC 2.0 ao vivo do MEditor (Etapa 3).
 *
 * Todos os métodos que mutam o estado da timeline/bin suportam dois modos:
 * 1. Execução imediata: cria `Fun undo, redo` locais e empilha via `pCore->pushUndo(undo, redo, label)`.
 * 2. Execução em lote (`undo.begin` ... `undo.commit`): acumula todas as lambdas `undo`/`redo`
 *    em `m_batchUndo` e `m_batchRedo` (ou via `MEditorApiServer::pushOrAccumulateUndo`),
 *    empilhando um único macro comando no `DocUndoStack` quando `undo.commit` é chamado.
 */
class Handlers : public QObject
{
    Q_OBJECT

public:
    using EventCallback = std::function<void(const QString &eventType, const QJsonObject &payload)>;
    using ScopeCallback = std::function<void(bool active)>;

    explicit Handlers(QObject *parent = nullptr);
    explicit Handlers(MEditorApiServer *server, QObject *parent = nullptr);
    ~Handlers() override = default;

    void setApiServer(MEditorApiServer *server);
    MEditorApiServer *apiServer() const { return m_apiServer; }

    /**
     * Registra callback para emissão de eventos assíncronos (ex.: `bin.clipReady`, `project.loaded`).
     */
    void setEventCallback(EventCallback cb);

    /**
     * Registra callback opcional para sinalizar ao `EventBridge` quando um comando da IA
     * (`origin: "claude"`) está em execução na UI thread.
     */
    void setAgentScopeCallback(ScopeCallback cb);

    /**
     * Despacha um método JSON-RPC 2.0 pelo nome.
     * Retorna `true` em caso de sucesso (preenchendo `outResult`), ou `false` em erro
     * (preenchendo `outErrorCode` e `outErrorMessage`).
     */
    bool dispatch(const QString &method, const QJsonObject &params, QJsonObject &outResult, int &outErrorCode,
                  QString &outErrorMessage);

    static bool dispatch(MEditorApiServer *server, const QString &method, const QJsonObject &params,
                         QJsonObject &outResult, int &outErrorCode, QString &outErrorMessage);

    /**
     * Verifica se há um lote `undo.begin` aberto.
     */
    bool isInUndoBatch() const { return m_inBatch; }

    // --- Controle de lote de Undo ---
    bool handleUndoBegin(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleUndoCommit(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleUndoRollback(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- Projeto ---
    bool handleProjectOpen(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleProjectNew(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleProjectSave(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- Bin ---
    bool handleBinImport(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- Timeline ---
    bool handleTimelineGet(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- Operações de Clip ---
    bool handleClipInsert(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleClipCut(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleClipResize(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleClipDelete(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleClipMove(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleClipSpeed(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- Marcadores (Guides), Legendas (Subtitles), Trilhas e Títulos ---
    bool handleMarkerAdd(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleSubtitleAdd(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleSubtitleEdit(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleSubtitleDelete(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleTrackAdd(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleTitleAdd(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleTitleEdit(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- UI e Playhead ---
    bool handleUiStatus(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handlePlayheadSeek(const QJsonObject &params, QJsonObject &outResult, QString &outError);

    // --- Utilitários extras ---
    bool handleUndo(const QJsonObject &params, QJsonObject &outResult, QString &outError);
    bool handleRedo(const QJsonObject &params, QJsonObject &outResult, QString &outError);

Q_SIGNALS:
    void eventEmitted(const QString &eventType, const QJsonObject &payload);

private:
    void emitEvent(const QString &eventType, const QJsonObject &payload);
    void finalizeOperationUndo(Fun &localUndo, Fun &localRedo, const QString &defaultLabel);

    bool m_inBatch{false};
    QString m_batchLabel;
    int m_batchOpCount{0};
    Fun m_batchUndo;
    Fun m_batchRedo;

    EventCallback m_eventCallback;
    ScopeCallback m_scopeCallback;
    QPointer<MEditorApiServer> m_apiServer;
};

} // namespace MEditor

using MEditorHandlers = MEditor::Handlers;
