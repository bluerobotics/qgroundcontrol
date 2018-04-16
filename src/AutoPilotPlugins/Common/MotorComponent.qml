/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick          2.3
import QtQuick.Controls 1.2
import QtQuick.Dialogs  1.2

import QGroundControl               1.0
import QGroundControl.Controls      1.0
import QGroundControl.FactSystem    1.0
import QGroundControl.ScreenTools   1.0

SetupPage {
    id:             motorPage
    pageComponent:  pageComponent

    visibleWhileArmed: true

    readonly property int _barHeight:       10
    readonly property int _barWidth:        5
    readonly property int _sliderHeight:    10

    property var neutralValue: 50;

    FactPanelController {
        id:             controller
        factPanel:      motorPage.viewPanel
    }

    function setMotorDirection(num, reversed) {
        var fact = controller.getParameterFact(-1, "MOT_" + num + "_DIRECTION")
        fact.value = reversed ? -1 : 1;
    }

    Component.onCompleted: controller.vehicle.armed = false

    Component {
        id: pageComponent

        Column {
            spacing: 10

            Row {
                id:         motorSliders
                enabled:    controller.vehicle.armed
                spacing:    ScreenTools.defaultFontPixelWidth * 4

                Column {
                    spacing:    ScreenTools.defaultFontPixelWidth * 2

                    Row {
                        id: sliderRow
                        spacing:    ScreenTools.defaultFontPixelWidth * 4

                        Repeater {
                            id:         sliderRepeater
                            model:      controller.vehicle.motorCount == -1 ? 8 : controller.vehicle.motorCount

                            Column {
                                property alias motorSlider: slider
                                spacing:    ScreenTools.defaultFontPixelWidth

                                Timer {
                                    interval:       250
                                    running:        true
                                    repeat:         true

                                    property real _lastValue: neutralValue // TODO should be called neutralValue, or actually implement last value?

                                    onTriggered: {
                                        if (controller.vehicle.armed) {
                                            controller.vehicle.motorTest(index, slider.value)
                                        }
                                    }
                                }

                                QGCLabel {
                                    anchors.horizontalCenter:   parent.horizontalCenter
                                    text:                       index + 1
                                }

                                QGCSlider {
                                    id:                         slider
                                    height:                     ScreenTools.defaultFontPixelHeight * _sliderHeight
                                    orientation:                Qt.Vertical
                                    maximumValue:               100
                                    value:                      neutralValue
                                }
                            } // Column
                        } // Repeater
                    } // Row

                    QGCLabel {
                        width: parent.width
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        wrapMode:       Text.WordWrap
                        text:           qsTr("Reverse Motor Direction")
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignBottom
                    }
                    Rectangle {
                        anchors.margins: ScreenTools.defaultFontPixelWidth * 3
                        width:              parent.width
                        height:             1
                        color:              qgcPal.text
                    }
                    Row {
                        anchors.margins: ScreenTools.defaultFontPixelWidth

                        Repeater {
                            id:         cbRepeater
                            model:      controller.vehicle.motorCount == -1 ? 8 : controller.vehicle.motorCount

                            Column {
                                spacing:    ScreenTools.defaultFontPixelWidth

                                QGCCheckBox {
                                    width: sliderRow.width / (controller.vehicle.motorCount - 0.5)
                                    checked: controller.getParameterFact(-1, "MOT_" + (index + 1) + "_DIRECTION").value == -1
                                    onClicked: {
                                        sliderRepeater.itemAt(index).motorSlider.value = neutralValue
                                        setMotorDirection(index + 1, checked)
                                    }
                                }
                            } // Column
                        } // Repeater
                    } // Row
                }

                Column {
                    spacing:    ScreenTools.defaultFontPixelWidth

                    QGCLabel {
                        anchors.horizontalCenter:   parent.horizontalCenter
                        text:                       qsTr("All")
                    }

                    QGCSlider {
                        id:                         allSlider
                        height:                     ScreenTools.defaultFontPixelHeight * _sliderHeight
                        orientation:                Qt.Vertical
                        maximumValue:               100
                        value:                      neutralValue

                        onValueChanged: {
                            for (var sliderIndex=0; sliderIndex<sliderRepeater.count; sliderIndex++) {
                                sliderRepeater.itemAt(sliderIndex).motorSlider.value = allSlider.value
                            }
                        }
                    }
                } // Column

//                MultiRotorMotorDisplay {
//                    anchors.top:    parent.top
//                    anchors.bottom: parent.bottom
//                    width:          height
//                    motorCount:     controller.vehicle.motorCount
//                    xConfig:        controller.vehicle.xConfigMotors
//                    coaxial:        controller.vehicle.coaxialMotors
//                }

                APMSubMotorDisplay {
                    anchors.top:    parent.top
                    anchors.bottom: parent.bottom
                    width:          height
                    frameType: controller.getParameterFact(-1, "FRAME_CONFIG").value
                }
            } // Row

            QGCLabel {
                anchors.left:   parent.left
                anchors.right:  parent.right
                wrapMode:       Text.WordWrap
                text:           qsTr("Moving the sliders will cause the motors to spin. Make sure the motors and propellers are clear from obstructions! The direction of the motor rotation is dependent on how the three phases of the motor are physically connected to the ESCs (if any two wires are swapped, the direction of rotation will flip). Because we cannot guarantee what order the phases are connected, the motor directions must be configured in software. When a slider is moved DOWN, the thruster should push air/water TOWARD the cable entering the housing. Click the checkbox to reverse the direction of the corresponding thruster.")
            }

            Row {
                spacing: ScreenTools.defaultFontPixelWidth
                Switch {
                    id: safetySwitch
                    onClicked: {
                        controller.vehicle.armed = checked
                        checked = !checked // As crazy as this looks, it keeps things working the way they should see onArmedChanged below, that will take care of checked state
                    }
                }

                Connections {
                    target: controller.vehicle
                    onArmedChanged:
                    {
                        safetySwitch.checked = armed
                            if (!armed) {
                                for (var sliderIndex=0; sliderIndex<sliderRepeater.count; sliderIndex++) {
                                    sliderRepeater.itemAt(sliderIndex).motorSlider.value = neutralValue
                                }
                                allSlider.value = neutralValue
                            }
                        }
                }

                QGCLabel {
                    color:  qgcPal.warningText
                    text:   qsTr("Slide this switch to arm the vehicle and enable the motor test (CAUTION!)")
                }
            } // Row
        } // Column
    } // Component
} // SetupPahe
