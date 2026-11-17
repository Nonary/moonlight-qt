.pragma library

var background = "#0B0D10"
var surface = "#14171C"
var raised = "#1B1F26"
var control = "#2A2F38"
var accent = "#FFFFFF"
var text = "#F2F4F7"
var muted = "#B4BAC4"
var caption = "#8A919C"
var disabled = "#5C636E"
var outline = "#1AFFFFFF"
var ok = "#4CC38A"
var warning = "#E5A63B"
var error = "#F0616D"
var typeSizes = { display: 40, title: 28, heading: 22, body: 20, secondary: 18, caption: 16 }

function scale(width, height) { return Math.max(0.75, Math.min(3, Math.min(width / 1280, height / 720))) }
function windowScale(window) { return window ? scale(window.width, window.height) : 1 }
function px(value, window) { return Math.round(value * windowScale(window)) }
function typePx(value, window) { return Math.round(value * Math.max(0.875, windowScale(window))) }
function typeSize(role, window) { return typePx(typeSizes[role] || 20, window) }
function placeholder(source) { return !source || source.toString().indexOf("no_app_image.png") >= 0 }

