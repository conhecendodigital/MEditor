/*
    SPDX-FileCopyrightText: 2015 Jean-Baptiste Mardelle <jb@kdenlive.org>
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

import QtQuick.Controls
import QtQuick.Window
import QtQuick

import org.kde.ki18n

import org.kde.kdenlive as K

Item {
    id: root
    objectName: "root"

    SystemPalette { id: activePalette }

    // default size, but scalable by user
    height: 300; width: 400
    required property K.MonitorProxy controller
    property int viewType: K.SceneType.MonitorSceneDefault
    property string markerText
    property point profile: root.controller.profile
    property double zoom
    property point center
    property double scalex
    property double scaley
    property bool dropped: false
    property string fps: '-'
    property bool showMarkers: false
    property bool showTimecode: false
    property bool showFps: false
    property bool showAudiothumb: false
    property double offsetx : 0
    property double offsety : 0
    property int duration: 300
    property bool isClipMonitor: false
    property var canvasState: ({})
    property string inlineEditMode: "" // "", "title", "subtitle"
    property bool showCanvasGuides: true

    function refreshCanvasState() {
        if (!root.isClipMonitor && root.controller) {
            root.canvasState = root.controller.canvasOverlayState()
        }
    }

    Component.onCompleted: {
        root.controller.rulerHeight = 0
        root.refreshCanvasState()
    }

    Connections {
        target: root.controller
        function onPositionChanged() {
            root.refreshCanvasState()
        }
        function onCanvasOverlayChanged() {
            root.refreshCanvasState()
        }
    }

    FontMetrics {
        id: fontMetrics
        font: K.UiUtils.fixedFont
    }

    signal editCurrentMarker()
    signal startRecording(bool showCountDown)

    function updatePalette() {
        clipMonitorRuler.forceRepaint()
    }

    function startCountdown() {
        countDownLoader.source = "Countdown.qml"
    }
    function stopCountdown() {
        root.startRecording(false)
        countDownLoader.source = ""
    }

    MouseArea {
        id: barOverArea
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        anchors.fill: parent
        onWheel: wheel => {
            root.controller.seek(wheel.angleDelta.x + wheel.angleDelta.y, wheel.modifiers)
        }
        onEntered: {
            root.controller.setWidgetKeyBinding(KI18n.i18n("<b>Click</b> to play, <b>Double click</b> for fullscreen, <b>Hover right</b> for toolbar, <b>Wheel</b> or <b>arrows</b> to seek, <b>Ctrl wheel</b> to zoom"));
        }
        onExited: {
            root.controller.setWidgetKeyBinding();
        }
    }
    DropArea { //Drop area for effects
        id: effectArea
        anchors.fill: parent
        keys: ['kdenlive/effect']
        property string droppedData
        property string droppedDataSource
        onEntered: drag => {
            drag.acceptProposedAction()
            droppedData = drag.getDataAsString('kdenlive/effect')
            droppedDataSource = drag.getDataAsString('kdenlive/effectsource')
        }
        onDropped: {
            root.controller.addEffect(droppedData, droppedDataSource)
            droppedData = ""
            droppedDataSource = ""
        }
    }
    SceneToolBar {
        id: sceneToolBar
        anchors {
            right: parent.right
            top: parent.top
            topMargin: 4
            rightMargin: 4
            leftMargin: 4
        }
        monitorController: root.controller
        isClipMonitor: root.isClipMonitor
    }

    Item {
        height: root.height - root.controller.rulerHeight
        width: root.width
        id: monitorArea
        Item {
            id: frame
            objectName: "referenceframe"
            width: root.profile.x * root.scalex
            height: root.profile.y * root.scaley
            x: root.center.x - width / 2 - root.offsetx;
            y: root.center.y - height / 2 - root.offsety;

            K.MonitorOverlay {
                anchors.fill: frame
                color: K.KdenliveSettings.overlayColor
                overlayType: root.controller.overlayType
            }

            K.MonitorSafeZone {
                anchors.fill: frame
                showSafeZone: root.controller.showSafezone
                profile: root.controller.profile
            }

            Loader {
                id: countDownLoader
                anchors.fill: frame
            }

            Connections {
                target: countDownLoader.item

                function onStopCountdown() {
                    root.stopCountdown()
                }
            }

            // --- MEditor OpenCut / CapCut Interactive On-Video Canvas Zones ---
            Item {
                id: openCutCanvasOverlay
                anchors.fill: frame
                visible: !root.isClipMonitor && root.showCanvasGuides

                // 1. Top Zone: Full-Video Title (V3)
                Rectangle {
                    id: titleZoneBox
                    x: frame.width * 0.04
                    y: frame.height * 0.02
                    width: frame.width * 0.92
                    height: frame.height * 0.15
                    radius: 8
                    color: (titleZoneMouse.containsMouse || root.canvasState.selectedZone === "title") ? "#282563EB" : "transparent"
                    border.width: (titleZoneMouse.containsMouse || root.canvasState.selectedZone === "title") ? 2 : 1
                    border.color: (titleZoneMouse.containsMouse || root.canvasState.selectedZone === "title") ? "#60A5FA" : "#443B82F6"

                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: 4
                        height: 20
                        width: titleBadgeText.implicitWidth + 14
                        radius: 5
                        color: "#CC1D4ED8"
                        visible: titleZoneMouse.containsMouse || root.canvasState.selectedZone === "title" || root.canvasState.hasTitle
                        Text {
                            id: titleBadgeText
                            anchors.centerIn: parent
                            text: "✏️ Título V3 (Clique p/ editar no vídeo)"
                            color: "#FFFFFF"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }

                    MouseArea {
                        id: titleZoneMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.canvasState = root.controller.selectCanvasZone("title")
                            root.inlineEditMode = "title"
                            inlineEditorInput.text = (root.canvasState.titleText || "COMO CRIAR SEU AGENTE DE IA").replace(/\n/g, " | ")
                            inlineEditorInput.forceActiveFocus()
                        }
                    }
                }

                // 2. Upper-Middle Zone: Motion Graphic (V2)
                Rectangle {
                    id: motionZoneBox
                    x: frame.width * 0.04
                    y: frame.height * 0.18
                    width: frame.width * 0.92
                    height: frame.height * 0.24
                    radius: 8
                    color: (motionZoneMouse.containsMouse || root.canvasState.selectedZone === "motion") ? "#247C3AED" : "transparent"
                    border.width: (motionZoneMouse.containsMouse || root.canvasState.selectedZone === "motion") ? 2 : (root.canvasState.hasMotion ? 1 : 0)
                    border.color: (motionZoneMouse.containsMouse || root.canvasState.selectedZone === "motion") ? "#A78BFA" : "#558B5CF6"

                    Rectangle {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: 4
                        height: 20
                        width: motionBadgeText.implicitWidth + 14
                        radius: 5
                        color: "#CC6D28D9"
                        visible: motionZoneMouse.containsMouse || root.canvasState.selectedZone === "motion" || root.canvasState.hasMotion
                        Text {
                            id: motionBadgeText
                            anchors.centerIn: parent
                            text: root.canvasState.hasMotion ? ("🎬 Motion V2: " + root.canvasState.motionName) : "🎬 Trilha Motion V2"
                            color: "#FFFFFF"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }

                    MouseArea {
                        id: motionZoneMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.canvasState = root.controller.selectCanvasZone("motion")
                        }
                    }
                }

                // 3. Lower Zone: Camera Cut (V1)
                Rectangle {
                    id: cameraZoneBox
                    x: frame.width * 0.04
                    y: frame.height * 0.58
                    width: frame.width * 0.92
                    height: frame.height * 0.38
                    radius: 8
                    color: (cameraZoneMouse.containsMouse || root.canvasState.selectedZone === "camera") ? "#1A10B981" : "transparent"
                    border.width: (cameraZoneMouse.containsMouse || root.canvasState.selectedZone === "camera") ? 2 : 1
                    border.color: (cameraZoneMouse.containsMouse || root.canvasState.selectedZone === "camera") ? "#34D399" : "#3310B981"

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.margins: 6
                        height: 22
                        width: cameraBadgeText.implicitWidth + 16
                        radius: 6
                        color: "#CC047857"
                        visible: cameraZoneMouse.containsMouse || root.canvasState.selectedZone === "camera" || root.canvasState.hasCamera
                        Text {
                            id: cameraBadgeText
                            anchors.centerIn: parent
                            text: root.canvasState.hasCamera
                                ? ("🎥 Corte V1 (" + root.canvasState.cutIndex + "/" + root.canvasState.totalCuts + ") — Clique p/ selecionar")
                                : "🎥 Câmera V1"
                            color: "#FFFFFF"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }

                    MouseArea {
                        id: cameraZoneMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.canvasState = root.controller.selectCanvasZone("camera")
                        }
                    }
                }

                // 4. Center Draggable Subtitle Box (Legendas)
                Rectangle {
                    id: subtitleZoneBox
                    x: frame.width * 0.05
                    width: frame.width * 0.90
                    height: Math.max(42, frame.height * 0.11)
                    y: Math.max(frame.height * 0.12, Math.min(frame.height * 0.85, (root.canvasState.subNormY !== undefined ? root.canvasState.subNormY : 0.515) * frame.height - height / 2))
                    radius: 8
                    color: (subZoneMouse.containsMouse || subZoneMouse.pressed || root.canvasState.selectedZone === "subtitle") ? "#33F59E0B" : "transparent"
                    border.width: (subZoneMouse.containsMouse || subZoneMouse.pressed || root.canvasState.selectedZone === "subtitle") ? 2 : 1
                    border.color: (subZoneMouse.containsMouse || subZoneMouse.pressed || root.canvasState.selectedZone === "subtitle") ? "#FBBF24" : "#66F59E0B"

                    Rectangle {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: 4
                        height: 20
                        width: subBadgeText.implicitWidth + 14
                        radius: 5
                        color: "#CCD97706"
                        visible: subZoneMouse.containsMouse || subZoneMouse.pressed || root.canvasState.selectedZone === "subtitle" || root.canvasState.hasSubtitle
                        Text {
                            id: subBadgeText
                            anchors.centerIn: parent
                            text: "💬 Legenda [" + (root.canvasState.subtitleStyle || "Costura") + "] ↕ Arraste ou Clique"
                            color: "#FFFFFF"
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }

                    MouseArea {
                        id: subZoneMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        property real pressStartY: 0
                        property bool draggedVertically: false

                        onPressed: mouse => {
                            pressStartY = mouse.y
                            draggedVertically = false
                            root.canvasState = root.controller.selectCanvasZone("subtitle")
                        }
                        onPositionChanged: mouse => {
                            if (pressed) {
                                var dy = mouse.y - pressStartY
                                if (Math.abs(dy) > 4 || draggedVertically) {
                                    draggedVertically = true
                                    var centerInFrame = subtitleZoneBox.y + mouse.y
                                    var normY = Math.max(0.14, Math.min(0.86, centerInFrame / Math.max(1, frame.height)))
                                    root.controller.dragSubtitleToNormY(normY)
                                }
                            }
                        }
                        onReleased: {
                            if (!draggedVertically) {
                                root.inlineEditMode = "subtitle"
                                inlineEditorInput.text = root.canvasState.subtitleText || ""
                                inlineEditorInput.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
        Item {
            id: monitorOverlay
            anchors.fill: monitorArea

            Label {
                id: timecode
                font.family: fontMetrics.font.family
                font.pointSize: 1.5 * fontMetrics.font.pointSize
                objectName: "timecode"
                color: "#ffffff"
                padding: 2
                background: Rectangle {
                    color: root.controller.monitorIsActive ? "#DD006600": "#66000000"
                }
                text: root.controller.timecode
                visible: root.showTimecode
                anchors {
                    right: monitorOverlay.right
                    bottom: monitorOverlay.bottom
                }
            }
            Label {
                id: fpsdropped
                font.family: fontMetrics.font.family
                font.pointSize: 1.5 * fontMetrics.font.pointSize
                objectName: "fpsdropped"
                color: "#ffffff"
                padding: 2
                background: Rectangle {
                    color: root.dropped ? "#99ff0000" : "#66004400"
                }
                text: KI18n.i18n("%1fps", root.fps)
                visible: root.showFps
                anchors {
                    right: timecode.visible ? timecode.left : parent.right
                    bottom: parent.bottom
                }
            }
            Label {
                id: labelSpeed
                font: K.UiUtils.fixedFont
                anchors {
                    left: parent.left
                    top: parent.top
                }
                visible: Math.abs(root.controller.speed) > 1
                text: "x" + root.controller.speed
                color: "white"
                background: Rectangle {
                    color: "darkgreen"
                }
                padding: 5
                horizontalAlignment: TextInput.AlignHCenter
            }
            Label {
                id: inPoint
                font: K.UiUtils.fixedFont
                anchors {
                    left: parent.left
                    bottom: parent.bottom
                }
                visible: root.showMarkers && root.controller.position == root.controller.zoneIn
                text: KI18n.i18n("In Point")
                color: "white"
                background: Rectangle {
                    color: "#228b22"
                }
                padding: 5
                horizontalAlignment: TextInput.AlignHCenter
            }
            Label {
                id: outPoint
                font: K.UiUtils.fixedFont
                anchors {
                    left: inPoint.visible ? inPoint.right : parent.left
                    bottom: parent.bottom
                }
                visible: root.showMarkers && root.controller.position == root.controller.zoneOut
                text: KI18n.i18n("Out Point")
                color: "white"
                background: Rectangle {
                    color: "#770000"
                }
                padding: 5
                horizontalAlignment: TextInput.AlignHCenter
            }
            TextField {
                id: marker
                font: K.UiUtils.fixedFont
                objectName: "markertext"
                activeFocusOnPress: true
                onEditingFinished: {
                    root.markerText = marker.displayText
                    marker.focus = false
                    root.editCurrentMarker()
                }
                anchors {
                    left: outPoint.visible ? outPoint.right : inPoint.visible ? inPoint.right : parent.left
                    bottom: parent.bottom
                }
                visible: root.showMarkers && text != ""
                text: root.controller.markerComment
                height: inPoint.height
                width: fontMetrics.boundingRect(displayText).width + 10
                horizontalAlignment: displayText == text ? TextInput.AlignHCenter : TextInput.AlignLeft
                background: Rectangle {
                    color: root.controller.markerColor
                }
                color: "#000000"
                padding: 0
                maximumLength: 25
            }
        }
        Rectangle {
            id: transformcontainer
            width: 2 * fontMetrics.font.pixelSize
            height: width
            anchors.top: monitorArea.top
            anchors.left: monitorArea.left
            anchors.topMargin: 10
            anchors.leftMargin: 10
            color: Qt.rgba(activePalette.window.r, activePalette.window.g, activePalette.window.b, 0.5)
            visible: K.KdenliveSettings.enableBuiltInEffects && root.controller.speed == 0 && (barOverArea.containsMouse || transformbutton.hovered)
            radius: 4
            border.color : Qt.rgba(0, 0, 0, 0.3)
            border.width: 1
            K.MonitorToolButton {
                id: transformbutton
                anchors.fill: transformcontainer
                hoverEnabled: true
                iconName: "transform-crop"
                toolTipText: KI18n.i18nc("@tooltip Transform, a tool to resize", "Enable Transform")
                checkable: false
                onClicked: {
                    root.controller.enableTransform()
                }
            }
        }

        // --- Inline On-Video Editor Modal (Title V3 or Subtitle) ---
        Rectangle {
            id: inlineCanvasEditorCard
            visible: !root.isClipMonitor && root.inlineEditMode !== ""
            anchors.horizontalCenter: monitorArea.horizontalCenter
            anchors.top: monitorArea.top
            anchors.topMargin: 14
            width: Math.min(monitorArea.width - 24, 520)
            height: 82
            radius: 10
            color: "#EE111827"
            border.width: 2
            border.color: root.inlineEditMode === "title" ? "#3B82F6" : "#F59E0B"
            z: 50

            Column {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                Text {
                    text: root.inlineEditMode === "title"
                        ? "✏️ Editar Título do Vídeo Inteiro (Trilha V3 — use | para quebrar linha):"
                        : "💬 Editar Texto da Legenda Atual no Vídeo:"
                    color: "#F9FAFB"
                    font.pixelSize: 12
                    font.bold: true
                }

                Row {
                    spacing: 6
                    width: parent.width

                    TextField {
                        id: inlineEditorInput
                        width: parent.width - 148
                        height: 32
                        color: "#FFFFFF"
                        font.pixelSize: 13
                        selectByMouse: true
                        background: Rectangle {
                            color: "#1F2937"
                            radius: 6
                            border.color: "#4B5563"
                            border.width: 1
                        }
                        onAccepted: {
                            if (root.inlineEditMode === "title") {
                                root.controller.updateFullVideoTitleText(inlineEditorInput.text.replace(/\s*\|\s*/g, "\n"))
                            } else if (root.inlineEditMode === "subtitle") {
                                root.controller.updateActiveSubtitleText(inlineEditorInput.text)
                            }
                            root.inlineEditMode = ""
                            root.refreshCanvasState()
                        }
                    }

                    Button {
                        width: 72
                        height: 32
                        text: "Salvar"
                        onClicked: {
                            if (root.inlineEditMode === "title") {
                                root.controller.updateFullVideoTitleText(inlineEditorInput.text.replace(/\s*\|\s*/g, "\n"))
                            } else if (root.inlineEditMode === "subtitle") {
                                root.controller.updateActiveSubtitleText(inlineEditorInput.text)
                            }
                            root.inlineEditMode = ""
                            root.refreshCanvasState()
                        }
                    }

                    Button {
                        width: 62
                        height: 32
                        text: "Fechar"
                        onClicked: {
                            root.inlineEditMode = ""
                        }
                    }
                }
            }
        }

        // --- Floating OpenCut / CapCut Quick-Action Pill Bar on Video ---
        Rectangle {
            id: openCutFloatingBar
            visible: !root.isClipMonitor
            anchors.horizontalCenter: monitorArea.horizontalCenter
            anchors.bottom: monitorArea.bottom
            anchors.bottomMargin: 8
            height: 36
            width: Math.min(monitorArea.width - 12, actionRow.implicitWidth + 16)
            radius: 10
            color: "#E60F172A"
            border.width: 1
            border.color: "#334155"
            z: 40

            Row {
                id: actionRow
                anchors.centerIn: parent
                spacing: 4

                Button {
                    height: 26
                    text: "⏮ Corte"
                    ToolTip.visible: hovered
                    ToolTip.text: "Ir para o corte anterior na timeline"
                    onClicked: root.controller.jumpPrevCut()
                }
                Button {
                    height: 26
                    text: "⏯ Play"
                    ToolTip.visible: hovered
                    ToolTip.text: "Play / Pause do vídeo"
                    onClicked: root.controller.togglePlay()
                }
                Button {
                    height: 26
                    text: "⏭ Corte"
                    ToolTip.visible: hovered
                    ToolTip.text: "Ir para o próximo corte na timeline"
                    onClicked: root.controller.jumpNextCut()
                }
                Button {
                    height: 26
                    text: "✂️ Cortar"
                    ToolTip.visible: hovered
                    ToolTip.text: "Dividir o corte atual exatamente na agulha"
                    onClicked: root.controller.splitActiveClipAtPlayhead()
                }
                Button {
                    height: 26
                    text: "⏪ -0.15s"
                    ToolTip.visible: hovered
                    ToolTip.text: "Encurtar corte atual em 0.15s (Ripple Magnético)"
                    onClicked: root.controller.rippleTrimActiveCut(-9)
                }
                Button {
                    height: 26
                    text: "⏩ +0.15s"
                    ToolTip.visible: hovered
                    ToolTip.text: "Esticar corte atual em +0.15s (Ripple Magnético)"
                    onClicked: root.controller.rippleTrimActiveCut(9)
                }
                Button {
                    height: 26
                    text: "🎨 " + (root.canvasState.subtitleStyle || "Costura")
                    ToolTip.visible: hovered
                    ToolTip.text: "Trocar preset de estilo de TODAS as legendas (CapCut / Editor IA)"
                    onClicked: root.controller.cycleSubtitlePreset()
                }
                Button {
                    height: 26
                    text: "🔤 Fonte"
                    ToolTip.visible: hovered
                    ToolTip.text: "Escolher fonte para todas as legendas"
                    onClicked: root.controller.openSubtitleFontDialog()
                }
                Button {
                    height: 26
                    text: "✏️ Título"
                    ToolTip.visible: hovered
                    ToolTip.text: "Editar o título do vídeo inteiro direto no preview"
                    onClicked: {
                        root.canvasState = root.controller.selectCanvasZone("title")
                        root.inlineEditMode = (root.inlineEditMode === "title") ? "" : "title"
                        inlineEditorInput.text = (root.canvasState.titleText || "COMO CRIAR SEU AGENTE DE IA").replace(/\n/g, " | ")
                        inlineEditorInput.forceActiveFocus()
                    }
                }
                Button {
                    height: 26
                    text: root.showCanvasGuides ? "👁 Guias ON" : "👁 Guias OFF"
                    ToolTip.visible: hovered
                    ToolTip.text: "Mostrar/ocultar caixas interativas no vídeo"
                    onClicked: root.showCanvasGuides = !root.showCanvasGuides
                }
            }
        }
    }
    MonitorRuler {
        id: clipMonitorRuler
        anchors {
            left: root.left
            right: root.right
            bottom: root.bottom
        }
        height: root.controller.rulerHeight
        monitorController: root.controller
        duration: root.duration
    }
}
