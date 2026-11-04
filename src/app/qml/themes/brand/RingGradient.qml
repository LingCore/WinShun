import QtQuick
import QtQuick.Shapes

// The Logo colours round a rounded box, stops placed by perimeter length
// (Look.ringStops). One set of GradientStop objects, made once and recoloured
// when the size or the theme changes.
ConicalGradient {
    id: gradient

    property QtObject look
    property real boxWidth
    property real boxHeight
    property int count: 72

    angle: 90 // 12 o'clock; Qt runs counter-clockwise from here, Look.ringStops allows for it
    centerX: boxWidth / 2
    centerY: boxHeight / 2

    readonly property var plan: look ? look.ringStops(boxWidth, boxHeight, look.dark, count) : []
    property var made: []

    onPlanChanged: {
        if (plan.length === 0)
            return
        if (made.length !== plan.length) {
            const list = []
            for (let i = 0; i < plan.length; i++)
                list.push(stopMaker.createObject(gradient, { position: plan[i].position, color: plan[i].color }))
            made = list
            gradient.stops = list
            return
        }
        for (let j = 0; j < plan.length; j++) {
            made[j].position = plan[j].position
            made[j].color = plan[j].color
        }
    }

    property Component stopMaker: Component { GradientStop {} }
}
