imgtag = {
  type: "home",
  afterid: "0",
  pegcount: 20
}

imgjson = {}

function send() {
  fetch('api/tagimginfor', {
    method: 'POST',
    body: JSON.stringify(imgtag)
  })
    .then(res => res.json())
    .then(json => imgjson = json)
    .then(_ => addimg())
}

function addimg() {
  var imgshow = document.getElementById("img")
  for (let imginfor of imgjson) {
    var img = document.createElement("img")
    img.src = "img/" + imginfor.id
    img.alt = "the apple is bad img"
    imgshow.appendChild(img);
  }
}

document.getElementById("next").onclick = function () {
  document.getElementById("img").replaceChildren()
  var len = imgjson.length
  if (len != undefined) {
    imgtag.afterid = imgjson[len - 1].id
  } else {
  }
  send()
}
