(() => {
  if (window.__rayforceAiAssistLoaded) return;
  window.__rayforceAiAssistLoaded = true;

  const API_URL = 'https://ai-assist.rayforcedb.com/api/ask-rayforce';

  function escapeHtml(value) {
    return String(value)
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;')
      .replaceAll("'", '&#39;');
  }

  function linkifyUrl(rawUrl) {
    const trailing = (rawUrl.match(/[),.;:!?]+$/) || [''])[0];
    const url = trailing ? rawUrl.slice(0, -trailing.length) : rawUrl;
    return `<a href="${url}" target="_blank" rel="noopener noreferrer">${url}</a>${trailing}`;
  }

  function inlineMarkdown(text) {
    let s = escapeHtml(text);
    const links = [];
    s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, (_, label, url) => {
      const token = `@@RFAI_LINK_${links.length}@@`;
      links.push(`<a href="${url}" target="_blank" rel="noopener noreferrer">${label}</a>`);
      return token;
    });
    s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
    s = s.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
    s = s.replace(/(^|[\s([])\[?(https?:\/\/[^\s<\]]+)/g, (_, prefix, url) => `${prefix}${linkifyUrl(url)}`);
    links.forEach((html, index) => { s = s.replace(`@@RFAI_LINK_${index}@@`, html); });
    return s;
  }

  function isTableRow(line) {
    const trimmed = line.trim();
    return trimmed.includes('|') && trimmed.split('|').length >= 3;
  }

  function isTableSeparator(line) {
    return /^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(line);
  }

  function tableCells(line) {
    let trimmed = line.trim();
    if (trimmed.startsWith('|')) trimmed = trimmed.slice(1);
    if (trimmed.endsWith('|')) trimmed = trimmed.slice(0, -1);
    return trimmed.split('|').map(cell => cell.trim());
  }

  function renderTable(rows) {
    if (rows.length < 2) return '';
    const header = tableCells(rows[0]);
    const bodyRows = rows.slice(isTableSeparator(rows[1]) ? 2 : 1).filter(isTableRow);
    const thead = `<thead><tr>${header.map(cell => `<th>${inlineMarkdown(cell)}</th>`).join('')}</tr></thead>`;
    const tbody = `<tbody>${bodyRows.map(row => `<tr>${tableCells(row).map(cell => `<td>${inlineMarkdown(cell)}</td>`).join('')}</tr>`).join('')}</tbody>`;
    return `<div class="rfai-table-wrap"><table>${thead}${tbody}</table></div>`;
  }

  function renderMarkdownish(markdown) {
    const lines = String(markdown || '').replace(/\r\n/g, '\n').split('\n');
    const html = [];
    let paragraph = [];
    let list = [];
    let table = [];
    let inCode = false;
    let code = [];

    function flushParagraph() {
      if (!paragraph.length) return;
      html.push(`<p>${inlineMarkdown(paragraph.join(' '))}</p>`);
      paragraph = [];
    }
    function flushList() {
      if (!list.length) return;
      html.push(`<ul>${list.map(item => `<li>${inlineMarkdown(item)}</li>`).join('')}</ul>`);
      list = [];
    }
    function flushTable() {
      if (!table.length) return;
      const rendered = renderTable(table);
      if (rendered) html.push(rendered);
      else html.push(`<p>${inlineMarkdown(table.join(' '))}</p>`);
      table = [];
    }
    function flushCode() {
      if (!code.length) return;
      html.push(`<pre><code>${escapeHtml(code.join('\n'))}</code></pre>`);
      code = [];
    }

    for (let i = 0; i < lines.length; i++) {
      const raw = lines[i];
      const line = raw.trimEnd();
      if (line.trim().startsWith('```')) {
        if (inCode) {
          inCode = false;
          flushCode();
        } else {
          flushParagraph(); flushList(); flushTable(); inCode = true;
        }
        continue;
      }
      if (inCode) { code.push(raw); continue; }

      const next = lines[i + 1] || '';
      const startsTable = isTableRow(line) && (isTableSeparator(next) || table.length > 0);
      if (startsTable || (table.length && isTableRow(line))) {
        flushParagraph(); flushList(); table.push(line); continue;
      }
      if (table.length) flushTable();

      if (!line.trim()) { flushParagraph(); flushList(); continue; }
      const heading = line.match(/^(#{1,3})\s+(.+)$/);
      if (heading) {
        flushParagraph(); flushList();
        const level = Math.min(3, heading[1].length + 2);
        html.push(`<h${level}>${inlineMarkdown(heading[2])}</h${level}>`);
        continue;
      }
      const bullet = line.match(/^[-*]\s+(.+)$/) || line.match(/^\d+\.\s+(.+)$/);
      if (bullet) { flushParagraph(); list.push(bullet[1]); continue; }
      paragraph.push(line.trim());
    }
    flushParagraph(); flushList(); flushTable(); flushCode();
    return html.join('') || '<p></p>';
  }

  function buildWidget() {
    let launcher = document.querySelector('#rfai-launcher');
    if (!launcher) {
      launcher = document.createElement('button');
      launcher.id = 'rfai-launcher';
      launcher.className = 'rfai-launcher';
      launcher.type = 'button';
      launcher.setAttribute('aria-haspopup', 'dialog');
      launcher.setAttribute('aria-controls', 'rfai-dialog');
      launcher.setAttribute('aria-expanded', 'false');
      launcher.innerHTML = `
        <span class="rfai-launcher-icon" aria-hidden="true">
          <svg viewBox="0 0 24 24" role="img" focusable="false">
            <path d="M12 2.4l1.72 5.3a2.9 2.9 0 0 0 1.86 1.86l5.3 1.72-5.3 1.72a2.9 2.9 0 0 0-1.86 1.86L12 20.16l-1.72-5.3A2.9 2.9 0 0 0 8.42 13l-5.3-1.72 5.3-1.72a2.9 2.9 0 0 0 1.86-1.86L12 2.4Z"/>
            <path d="M18.8 2.8l.56 1.72c.1.3.34.54.64.64l1.72.56-1.72.56c-.3.1-.54.34-.64.64l-.56 1.72-.56-1.72a1 1 0 0 0-.64-.64l-1.72-.56 1.72-.56a1 1 0 0 0 .64-.64l.56-1.72Z"/>
          </svg>
        </span>
        <span>Ask AI</span>`;
    }

    const dialog = document.createElement('section');
    dialog.id = 'rfai-dialog';
    dialog.className = 'rfai-dialog';
    dialog.setAttribute('role', 'dialog');
    dialog.setAttribute('aria-modal', 'false');
    dialog.setAttribute('aria-labelledby', 'rfai-title');
    dialog.hidden = true;
    dialog.innerHTML = `
      <header class="rfai-header">
        <div class="rfai-title-row">
          <img class="rfai-logo" src="/assets/logo-light-full.svg" alt="Rayforce">
          <div>
            <h2 id="rfai-title">Ask AI</h2>
            <p>Ask about RayforceDB</p>
          </div>
        </div>
        <button id="rfai-close" class="rfai-icon-button" type="button" aria-label="Close Ask AI">
          <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M6.4 6.4 17.6 17.6M17.6 6.4 6.4 17.6"/></svg>
        </button>
      </header>
      <div id="rfai-messages" class="rfai-messages" aria-live="polite">
        <div class="rfai-msg rfai-msg-assistant rfai-welcome"><div class="rfai-content"><p>Ask a question about RayforceDB internals, architecture, performance, or usage.</p></div></div>
        <div class="rfai-suggestions" aria-label="Suggested questions">
          <button type="button" data-rfai-question="Where is query execution implemented in RayforceDB?">Query execution</button>
          <button type="button" data-rfai-question="Explain RayforceDB architecture briefly.">Architecture</button>
          <button type="button" data-rfai-question="Give me links to the Quick Start and Tutorial documentation.">Docs links</button>
        </div>
      </div>
      <form id="rfai-form" class="rfai-form">
        <textarea id="rfai-question" class="rfai-question" maxlength="1200" rows="1" placeholder="Ask me a question about RayforceDB…" required></textarea>
        <button id="rfai-submit" class="rfai-send" type="submit" aria-label="Send question">↑</button>
      </form>`;

    if (!launcher.isConnected) document.body.appendChild(launcher);
    document.body.appendChild(dialog);
    return {
      launcher,
      dialog,
      closeButton: dialog.querySelector('#rfai-close'),
      form: dialog.querySelector('#rfai-form'),
      question: dialog.querySelector('#rfai-question'),
      submit: dialog.querySelector('#rfai-submit'),
      messages: dialog.querySelector('#rfai-messages')
    };
  }

  function init() {
    const ui = buildWidget();

    function scrollMessagesToBottom() { ui.messages.scrollTop = ui.messages.scrollHeight; }
    function openDialog() {
      ui.dialog.hidden = false;
      ui.launcher.setAttribute('aria-expanded', 'true');
      setTimeout(() => ui.question.focus(), 0);
    }
    function closeDialog() {
      ui.dialog.hidden = true;
      ui.launcher.setAttribute('aria-expanded', 'false');
      ui.launcher.focus();
    }
    function addMessage(kind, body) {
      const div = document.createElement('div');
      div.className = `rfai-msg rfai-msg-${kind}`;
      const content = kind === 'assistant' ? renderMarkdownish(body) : `<p>${escapeHtml(body)}</p>`;
      div.innerHTML = `<div class="rfai-content">${content}</div>`;
      ui.messages.appendChild(div);
      scrollMessagesToBottom();
      return div;
    }
    function addErrorMessage(body, retryQuestion) {
      const div = document.createElement('div');
      div.className = 'rfai-msg rfai-msg-error';
      div.innerHTML = `
        <div class="rfai-content rfai-error-content">
          <p>${escapeHtml(body)}</p>
          <button class="rfai-retry-button" type="button" aria-label="Retry request" title="Retry request">
            <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false"><path d="M20 12a8 8 0 1 1-2.34-5.66"/><path d="M20 4.5v5h-5"/></svg>
          </button>
        </div>`;
      div.querySelector('.rfai-retry-button').addEventListener('click', () => {
        if (ui.submit.disabled) return;
        div.remove();
        askRayforce(retryQuestion, { showUser: false });
      });
      ui.messages.appendChild(div);
      scrollMessagesToBottom();
      return div;
    }

    async function askRayforce(text, options = {}) {
      const q = String(text || '').trim();
      if (!q) return;
      const { showUser = true } = options;
      openDialog();
      if (showUser) addMessage('user', q);
      ui.question.value = '';
      ui.submit.disabled = true;
      ui.submit.textContent = '…';
      const pending = addMessage('assistant', 'Thinking…');
      try {
        const res = await fetch(API_URL, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ question: q })
        });
        const data = await res.json();
        pending.remove();
        if (!res.ok || data.error) {
          addErrorMessage(data.error || `HTTP ${res.status}`, q);
          return;
        }
        addMessage('assistant', data.answer);
      } catch (_err) {
        pending.remove();
        addErrorMessage('Request failed. Please try again.', q);
      } finally {
        ui.submit.disabled = false;
        ui.submit.textContent = '↑';
        ui.question.focus();
      }
    }

    ui.launcher.addEventListener('click', () => {
      if (ui.dialog.hidden) openDialog();
      else closeDialog();
    });
    ui.closeButton.addEventListener('click', closeDialog);
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && !ui.dialog.hidden) closeDialog();
    });
    ui.dialog.querySelectorAll('[data-rfai-question]').forEach(button => {
      button.addEventListener('click', () => askRayforce(button.dataset.rfaiQuestion));
    });
    ui.question.addEventListener('keydown', (event) => {
      if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) {
        event.preventDefault();
        ui.form.requestSubmit();
      }
    });
    ui.form.addEventListener('submit', (event) => {
      event.preventDefault();
      askRayforce(ui.question.value);
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init, { once: true });
  } else {
    init();
  }
})();
