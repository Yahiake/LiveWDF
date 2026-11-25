"use strict";

const catalog = document.querySelector("#preset-table");
const tbody = document.querySelector("#preset-rows");
const count = document.querySelector("#preset-count");
const search = document.querySelector("#preset-search");
const collection = document.querySelector("#preset-collection");
const type = document.querySelector("#preset-type");
const headers = Array.from(catalog.querySelectorAll("button[data-sort]"));
const collator = new Intl.Collator("en", { numeric: true, sensitivity: "base" });
const timings = new Intl.NumberFormat("en", { maximumFractionDigits: 1 });
const schematicRoot = "https://github.com/Yahiake/LiveWDF/blob/main/schematics/";
const timingColumns = ["mean_us", "median_us", "p99_us"];
const blockBudgetUs = 128 / 48000 * 1e6;

let presets = [];
let sortColumn = "median_us";
let ascending = true;

function appendOptions(select, key) {
  const values = Array.from(new Set(presets.map(preset => preset[key])));
  for (const value of values.sort(collator.compare)) {
    select.add(new Option(value, value));
  }
}

function rowFor(preset) {
  const row = document.createElement("tr");
  const nameCell = row.insertCell();
  const link = document.createElement("a");
  link.className = "preset-name";
  link.href = schematicRoot + preset.file.split("/").map(encodeURIComponent).join("/");
  link.textContent = preset.name;
  const description = document.createElement("span");
  description.className = "preset-description";
  description.textContent = preset.description;
  nameCell.append(link, description);
  row.insertCell().textContent = preset.collection;
  row.insertCell().textContent = preset.type;
  for (const column of timingColumns) {
    const cell = row.insertCell();
    cell.className = "timing";
    cell.textContent = timings.format(preset[column]);
    if (column === "p99_us" && preset[column] > blockBudgetUs) {
      cell.classList.add("over-budget");
      cell.title = "Over the 2.67 ms block budget";
    }
  }
  return row;
}

function render() {
  const query = search.value.trim().toLowerCase().split(/\s+/);
  const matching = presets.filter(preset => {
    const text = [preset.name, preset.description, preset.collection, preset.type].join(" ").toLowerCase();
    return (!collection.value || preset.collection === collection.value) &&
           (!type.value || preset.type === type.value) &&
           query.every(word => text.includes(word));
  });
  matching.sort((a, b) => {
    const x = a[sortColumn];
    const y = b[sortColumn];
    const order = typeof x === "number" ? x - y : collator.compare(x, y);
    return (ascending ? order : -order) || collator.compare(a.name, b.name);
  });

  tbody.replaceChildren(...matching.map(rowFor));
  if (matching.length === 0) {
    const cell = tbody.insertRow().insertCell();
    cell.colSpan = headers.length;
    cell.className = "empty-row";
    cell.textContent = "No presets match these filters.";
  }
  count.textContent = matching.length === presets.length
    ? presets.length + " presets"
    : matching.length + " of " + presets.length + " presets";

  for (const button of headers) {
    if (button.dataset.sort === sortColumn) {
      button.parentElement.setAttribute("aria-sort", ascending ? "ascending" : "descending");
    } else {
      button.parentElement.removeAttribute("aria-sort");
    }
  }
}

async function loadCatalog() {
  try {
    const response = await fetch("presets.json");
    if (!response.ok) throw new Error("Could not load presets");
    presets = await response.json();
    appendOptions(collection, "collection");
    appendOptions(type, "type");
    render();

    search.addEventListener("input", render);
    collection.addEventListener("change", render);
    type.addEventListener("change", render);
    document.querySelector("#clear-filters").addEventListener("click", () => {
      search.value = "";
      collection.value = "";
      type.value = "";
      render();
      search.focus();
    });
    for (const button of headers) {
      button.addEventListener("click", () => {
        ascending = sortColumn === button.dataset.sort ? !ascending : true;
        sortColumn = button.dataset.sort;
        render();
      });
    }
  } catch {
    count.textContent = "Preset timings are unavailable. See the GitHub Markdown catalog below.";
  }
}

for (const audio of document.querySelectorAll("audio")) {
  audio.addEventListener("play", () => {
    for (const other of document.querySelectorAll("audio")) {
      if (other !== audio) other.pause();
    }
  });
}

loadCatalog();
