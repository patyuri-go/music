function add_path() {
  const input = document.getElementById("path_input").value.trim()
  const url = `/api/addpath?path=${encodeURIComponent(input)}`;
  fetch(url);
}
