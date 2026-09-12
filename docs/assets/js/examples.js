(function () {
  "use strict";

  var labels = {python: "Python", r: "R", julia: "Julia", shell: "Shell"};
  var preferred = "python";

  try {
    preferred = localStorage.getItem("rumi-example-language") || preferred;
  } catch (_error) {
    // The examples still work when file:// storage is unavailable.
  }

  function writeClipboard(value) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      return navigator.clipboard.writeText(value);
    }
    return new Promise(function (resolve, reject) {
      var area = document.createElement("textarea");
      area.value = value;
      area.setAttribute("readonly", "");
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      try {
        if (!document.execCommand("copy")) throw new Error("copy failed");
        resolve();
      } catch (error) {
        reject(error);
      } finally {
        area.remove();
      }
    });
  }

  function copyButton(getText) {
    var button = document.createElement("button");
    button.className = "code-copy";
    button.type = "button";
    button.textContent = "Copy";
    button.addEventListener("click", function () {
      writeClipboard(getText()).then(function () {
        button.textContent = "Copied";
        button.classList.add("is-copied");
        window.setTimeout(function () {
          button.textContent = "Copy";
          button.classList.remove("is-copied");
        }, 1400);
      }).catch(function () { button.textContent = "Select"; });
    });
    return button;
  }

  function escapeHtml(value) {
    return value.replace(/[&<>]/g, function (character) {
      return {"&": "&amp;", "<": "&lt;", ">": "&gt;"}[character];
    });
  }

  function highlightCode(code, language) {
    if (!code || code.dataset.highlighted) return;

    var keywordSets = {
      python: "and|as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield",
      r: "break|else|for|function|if|in|next|repeat|return|while|library",
      julia: "baremodule|begin|break|catch|const|continue|do|else|elseif|end|export|finally|for|function|global|if|import|let|local|macro|module|quote|return|struct|try|using|while|where",
      shell: "case|do|done|elif|else|esac|fi|for|function|if|in|then|until|while"
    };
    var normalized = (language || "text").toLowerCase();
    var keywords = keywordSets[normalized] || keywordSets.python;
    var pattern = new RegExp(
      "(#.*$)" +
      "|(\\\"\\\"\\\"[\\s\\S]*?\\\"\\\"\\\"|'''[\\s\\S]*?'''|\\\"(?:\\\\.|[^\\\"\\\\])*\\\"|'(?:\\\\.|[^'\\\\])*')" +
      "|(\\b(?:0x[\\da-fA-F]+|\\d+(?:\\.\\d+)?)\\b)" +
      "|(\\b(?:" + keywords + ")\\b)" +
      "|(\\b(?:True|False|None|NULL|NA|NaN|Inf|missing|nothing|true|false)\\b)" +
      "|(\\b[A-Za-z_]\\w*(?=\\s*\\())" +
      "|(<-|->|=>|==|!=|<=|>=|::|\\|>|&&|\\|\\|)",
      "gm"
    );
    var source = code.textContent;
    var cursor = 0;
    var html = "";

    source.replace(pattern, function (match, comment, string, number, keyword, constant, fn, operator, offset) {
      html += escapeHtml(source.slice(cursor, offset));
      var kind = comment ? "comment" : string ? "string" : number ? "number" :
        keyword ? "keyword" : constant ? "constant" : fn ? "function" : "operator";
      html += '<span class="syntax-' + kind + '">' + escapeHtml(match) + "</span>";
      cursor = offset + match.length;
      return match;
    });
    html += escapeHtml(source.slice(cursor));
    code.innerHTML = html;
    code.dataset.highlighted = "true";
  }

  function enhanceCodeCard(pre) {
    if (pre.closest("[data-code-group]")) return;

    var language = pre.dataset.language || "Text";
    highlightCode(pre.querySelector("code"), language);
    var card = document.createElement("div");
    card.className = "code-card";
    var bar = document.createElement("div");
    bar.className = "code-toolbar";
    var name = document.createElement("span");
    name.className = "code-language";
    name.textContent = language;

    pre.parentNode.insertBefore(card, pre);
    card.appendChild(bar);
    card.appendChild(pre);
    bar.appendChild(name);
    bar.appendChild(copyButton(function () { return pre.textContent; }));
  }

  function enhanceCodeGroup(group, groupIndex) {
    var panels = Array.prototype.slice.call(
      group.querySelectorAll(":scope > [data-language-panel]")
    );
    if (!panels.length) return;

    panels.forEach(function (panel) {
      highlightCode(panel.querySelector("code"), panel.dataset.languagePanel);
    });

    var bar = document.createElement("div");
    bar.className = "code-toolbar code-toolbar--tabs";
    var tabs = document.createElement("div");
    tabs.className = "code-tabs";
    tabs.setAttribute("role", "tablist");
    tabs.setAttribute("aria-label", "Example language");
    var copy;

    function selectedPanel() {
      return panels.find(function (panel) { return !panel.hidden; });
    }

    function activate(language, focus) {
      var wanted = panels.find(function (panel) {
        return panel.dataset.languagePanel === language;
      }) || panels[0];

      panels.forEach(function (panel) {
        var active = panel === wanted;
        panel.hidden = !active;
        var tab = tabs.querySelector('[data-language-tab="' + panel.dataset.languagePanel + '"]');
        tab.setAttribute("aria-selected", active ? "true" : "false");
        tab.tabIndex = active ? 0 : -1;
        if (active && focus) tab.focus();
      });

      preferred = wanted.dataset.languagePanel;
      try { localStorage.setItem("rumi-example-language", preferred); } catch (_error) {}
      if (copy) copy.hidden = wanted.hasAttribute("data-coming-soon");
    }

    panels.forEach(function (panel, panelIndex) {
      var language = panel.dataset.languagePanel;
      var tab = document.createElement("button");
      var panelId = "code-panel-" + groupIndex + "-" + panelIndex;
      var tabId = "code-tab-" + groupIndex + "-" + panelIndex;
      tab.type = "button";
      tab.id = tabId;
      tab.className = "code-tab";
      tab.dataset.languageTab = language;
      tab.setAttribute("role", "tab");
      tab.setAttribute("aria-controls", panelId);
      tab.textContent = labels[language] || language;
      panel.id = panelId;
      panel.setAttribute("role", "tabpanel");
      panel.setAttribute("aria-labelledby", tabId);
      tab.addEventListener("click", function () { activate(language, false); });
      tabs.appendChild(tab);
    });

    tabs.addEventListener("keydown", function (event) {
      var current = tabs.querySelector('[aria-selected="true"]');
      var buttons = Array.prototype.slice.call(tabs.querySelectorAll("button"));
      var index = buttons.indexOf(current);
      if (event.key === "ArrowRight") index = (index + 1) % buttons.length;
      else if (event.key === "ArrowLeft") index = (index - 1 + buttons.length) % buttons.length;
      else if (event.key === "Home") index = 0;
      else if (event.key === "End") index = buttons.length - 1;
      else return;
      event.preventDefault();
      activate(buttons[index].dataset.languageTab, true);
    });

    group.insertBefore(bar, group.firstChild);
    bar.appendChild(tabs);
    copy = copyButton(function () {
      var panel = selectedPanel();
      return panel ? panel.textContent.trim() : "";
    });
    bar.appendChild(copy);
    activate(preferred, false);
  }

  document.querySelectorAll("[data-code-group]").forEach(enhanceCodeGroup);
  document.querySelectorAll(".examples-page pre").forEach(enhanceCodeCard);
})();
