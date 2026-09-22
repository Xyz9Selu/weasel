#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled)
      : CEditSession(pTextService, pContext) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
};

STDMETHODIMP CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  HRESULT hrStart = pContextComposition->StartComposition(
      ec, pRangeComposition, _pTextService, &pComposition);
  if (FAILED(hrStart) || pComposition == NULL) {
    // Some hosts reject a composition over a non-empty range: fall back to
    // the old behavior (collapse to insertion point, then start).
    TF_SELECTION tfSelectionFallback;
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    tfSelectionFallback.range = pRangeComposition;
    tfSelectionFallback.style.ase = TF_AE_NONE;
    tfSelectionFallback.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelectionFallback);
    hrStart = pContextComposition->StartComposition(ec, pRangeComposition,
                                                   _pTextService, &pComposition);
  }
  if (SUCCEEDED(hrStart) && (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    if (!_pTextService->_TrackSelectionReplace(_pContext, ec, pRangeComposition,
                                               pComposition)) {
      /* empty selection: collapse to an insertion point (old behavior) */
      TF_SELECTION tfSelection;
      pRangeComposition->Collapse(ec, TF_ANCHOR_END);
      tfSelection.range = pRangeComposition;
      tfSelection.style.ase = TF_AE_NONE;
      tfSelection.style.fInterimChar = FALSE;
      _pContext->SetSelection(ec, 1, &tfSelection);
    }
    // else: composition covers the document selection, which stays selected.
    // Committing writes into the range (replacing it); cancelling puts the
    // tracked text back. No caret juggling needed.

    // The old composition's range is still visible while its asynchronous
    // end session is pending. Position only after the new composition has
    // actually been created, not from the response handler's stale range.
    _pTextService->_UpdateCompositionWindow(_pContext);
  }

  return hr;
}

/* Replace-selection tracking */
namespace {
// Bounded so a huge selection falls back to the old collapse behavior
// instead of allocating blindly.
const ULONG kMaxReplaceChars = 4096;
}  // namespace

BOOL WeaselTSF::_TrackSelectionReplace(com_ptr<ITfContext> pContext,
                                       TfEditCookie ec, ITfRange* pRange,
                                       ITfComposition* pComposition) {
  _UntrackSelectionReplace(nullptr);
  TF_SELECTION sel;
  ULONG fetched = 0;
  if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                                    &fetched)) ||
      fetched != 1) {
    return FALSE;
  }
  com_ptr<ITfRange> pSelection;
  pSelection.Attach(sel.range);
  LONG cmp = 0;
  // Empty selection: nothing to replace, keep the old collapse behavior.
  if (FAILED(pSelection->CompareStart(ec, pSelection, TF_ANCHOR_END, &cmp)) ||
      cmp == 0) {
    return FALSE;
  }
  // The composition range must be exactly the selection; otherwise fall back
  // to collapsing rather than covering the wrong text.
  if (FAILED(pRange->CompareStart(ec, pSelection, TF_ANCHOR_START, &cmp)) ||
      cmp != 0 ||
      FAILED(pRange->CompareEnd(ec, pSelection, TF_ANCHOR_END, &cmp)) ||
      cmp != 0) {
    return FALSE;
  }
  wchar_t buffer[kMaxReplaceChars];
  ULONG cch = 0;
  if (FAILED(pRange->GetText(ec, 0, buffer, kMaxReplaceChars - 1, &cch)) ||
      cch >= kMaxReplaceChars - 1) {
    return FALSE;
  }
  _replaceText.assign(buffer, cch);
  _pReplaceComposition = pComposition;
  _replaceSaved = TRUE;
  return TRUE;
}

BOOL WeaselTSF::_RestoreSelectionReplace(TfEditCookie ec,
                                         ITfComposition* pComposition,
                                         ITfRange* pRange) {
  if (!_replaceSaved || _pReplaceComposition == nullptr ||
      _pReplaceComposition != pComposition) {
    return FALSE;
  }
  wchar_t buffer[kMaxReplaceChars];
  ULONG cch = 0;
  if (SUCCEEDED(pRange->GetText(ec, 0, buffer, kMaxReplaceChars - 1, &cch)) &&
      cch == _replaceText.length() &&
      _replaceText.compare(0, cch, buffer, cch) == 0) {
    // Untouched (e.g. cancelled before anything was written): leave it,
    // skipping the clear that would otherwise delete the selection.
    return TRUE;
  }
  // Cleared already, or inline preedit replaced it live: put it back.
  // (If the host edited inside our range meanwhile, restoring the
  // selection is still preferable to deleting whatever is there.)
  pRange->SetText(ec, 0, _replaceText.c_str(),
                  static_cast<LONG>(_replaceText.length()));
  return TRUE;
}

void WeaselTSF::_UntrackSelectionReplace(ITfComposition* pComposition) {
  if (pComposition != nullptr) {
    if (!_replaceSaved || _pReplaceComposition == nullptr ||
        _pReplaceComposition != pComposition) {
      return;
    }
  }
  _replaceSaved = FALSE;
  _replaceText.clear();
  _pReplaceComposition = nullptr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                   BOOL fCUASWorkaroundEnabled) {
  DEBUG << "_StartComposition";
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(
      new CStartCompositionEditSession(this, pContext, fCUASWorkaroundEnabled));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE)
      : CEditSession(pTextService, pContext), _clear(clear) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
};

STDMETHODIMP CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr)
    return S_OK;
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK) {
    // A composition may cover a document selection (replace-selection):
    // put the tracked text back instead of eating it. Falls back to
    // clearing when nothing is tracked for this composition.
    if (!_pTextService->_RestoreSelectionReplace(ec, _pComposition,
                                                 pCompositionRange))
      pCompositionRange->SetText(ec, 0, L"", 0);
  }
  // The composition is ending either way: drop replace-selection tracking.
  _pTextService->_UntrackSelectionReplace(_pComposition);

  // Drop ownership before EndComposition(). Some applications notify
  // OnCompositionTerminated synchronously while the old composition ends.
  // Keeping it as the current composition makes that normal notification
  // look like an external abort and can clear a new Rime composition during
  // auto-commit.
  if (_pTextService && _pTextService->_IsCurrentComposition(_pComposition))
    _pTextService->_FinalizeComposition();
  _pComposition->EndComposition(ec);
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext,
                                BOOL clear,
                                BOOL endUI) {
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;
  com_ptr<ITfComposition> pComposition = _pComposition;

  if (endUI)
    _cand->EndUI();
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, pComposition, clear)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
};

STDMETHODIMP CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc;
  BOOL fClipped;
  TF_SELECTION selection;
  ULONG nSelection;

  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection)))
    return E_FAIL;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                     &nSelection)))
    return E_FAIL;

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end
    // note: selection.range is always an empty range
    pRange = selection.range;
  }

  if ((_pContextView->GetTextExt(ec, pRange, &rc, &fClipped)) == S_OK &&
      (rc.left != 0 || rc.top != 0)) {
    // get the foreground window pos and check if rc from GetTextExt is out of
    // window
    if (_enhancedPosition) {
      HWND hwnd;
      RECT rcForegroundWindow;
      hwnd = GetForegroundWindow();
      ::GetWindowRect(hwnd, &rcForegroundWindow);

      if (rc.left < rcForegroundWindow.left ||
          rc.left > rcForegroundWindow.right ||
          rc.top < rcForegroundWindow.top ||
          rc.top > rcForegroundWindow.bottom) {
        POINT pt;
        bool hasCaret = ::GetCaretPos(&pt);
        int offsetx = rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
        int offsety = rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
        rc.left += offsetx;
        rc.right += offsetx;
        rc.top += offsety;
        rc.bottom += offsety;
      }
    }
    _pTextService->_SetCompositionPosition(rc);
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDMETHODIMP CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
}

/* Composition State */
STDMETHODIMP WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                                 ITfComposition* pComposition) {
  DEBUG << "OnCompositionTerminated: current="
        << _IsCurrentComposition(pComposition)
        << " rime_composing=" << _status.composing;
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  // EndComposition() may generate this callback for the composition we just
  // closed. Only an active, matching composition is an external termination.
  if (!_IsCurrentComposition(pComposition))
    return S_OK;

  // A host may terminate the empty TSF composition used for a non-inline
  // preedit. Keep Rime's composing state; the next key will create a fresh
  // TSF composition. Only an inactive Rime session should be aborted here.
  if (_status.composing) {
    _FinalizeComposition();
    return S_OK;
  }

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  m_client.ClearComposition();
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  }
  _committed = TRUE;
  _cand->Destroy();
}

/* Commit Composition: insert committed text, then end the composition */
class CCommitCompositionEditSession : public CEditSession {
 public:
  CCommitCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                                com_ptr<ITfContext> pContext,
                                com_ptr<ITfComposition> pComposition,
                                const std::wstring& text)
      : CEditSession(pTextService, pContext), _text(text) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  std::wstring _text;
};

STDMETHODIMP CCommitCompositionEditSession::DoEditSession(TfEditCookie ec) {
  DEBUG << "CCommitCompositionEditSession: text_len=" << _text.length();
  if (_pComposition == nullptr) {
    DEBUG << "CCommitCompositionEditSession: null composition, skip";
    return S_OK;
  }
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext)
    return S_OK;

  com_ptr<ITfRange> pRange;
  if (FAILED(_pComposition->GetRange(&pRange))) {
    DEBUG << "CCommitCompositionEditSession: GetRange failed";
    return E_FAIL;
  }

  if (!_text.empty()) {
    HRESULT hrText = pRange->SetText(ec, 0, _text.c_str(),
                                     static_cast<LONG>(_text.length()));
    DEBUG << "CCommitCompositionEditSession: SetText hr=" << hrText;
    if (FAILED(hrText))
      return E_FAIL;
  } else {
    // Nothing was committed (e.g. server unreachable): drop any inline
    // preedit leftover so no stale text survives the switch.
    pRange->SetText(ec, 0, L"", 0);
  }

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);
  TF_SELECTION tfSelection;
  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  // Same ownership handover as CEndCompositionEditSession: drop the local
  // pointer before EndComposition() so a synchronous
  // OnCompositionTerminated for this composition is not mistaken for an
  // external abort.
  if (_pTextService->_IsCurrentComposition(_pComposition))
    _pTextService->_FinalizeComposition();
  DEBUG << "CCommitCompositionEditSession: EndComposition";
  _pComposition->EndComposition(ec);
  // The committed text already replaced the range content (including a
  // covered selection): just drop replace-selection tracking.
  _pTextService->_UntrackSelectionReplace(_pComposition);
  return S_OK;
}

/* Insert commit text at caret when no TSF composition exists */
class CCommitTextEditSession : public CEditSession {
 public:
  CCommitTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext), _text(text) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
};

STDMETHODIMP CCommitTextEditSession::DoEditSession(TfEditCookie ec) {
  DEBUG << "CCommitTextEditSession: text_len=" << _text.length();
  if (!_pTextService || !_pContext)
    return S_OK;
  if (_text.empty())
    return S_OK;

  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  if (FAILED(_pContext->QueryInterface(
          IID_ITfInsertAtSelection, (LPVOID*)&pInsertAtSelection)) ||
      pInsertAtSelection == nullptr) {
    DEBUG << "CCommitTextEditSession: no InsertAtSelection";
    return E_FAIL;
  }
  com_ptr<ITfRange> pRange;
  HRESULT hrIns = pInsertAtSelection->InsertTextAtSelection(
      ec, 0, _text.c_str(), static_cast<LONG>(_text.length()), &pRange);
  DEBUG << "CCommitTextEditSession: insert hr=" << hrIns;
  if (FAILED(hrIns))
    return hrIns;
  if (pRange != nullptr) {
    pRange->Collapse(ec, TF_ANCHOR_END);
    TF_SELECTION tfSelection;
    tfSelection.range = pRange;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);
  }
  return S_OK;
}

void WeaselTSF::_CommitComposition() {
  DEBUG << "_CommitComposition: tsf_composing=" << _IsComposing()
        << " rime_composing=" << _status.composing
        << " has_ctx=" << (_pEditSessionContext != nullptr)
        << " tsf_build=" __DATE__ " " __TIME__;
  // NOTE: the TSF composition and the Rime composition can diverge: the host
  // may terminate our (empty, non-inline) TSF composition while Rime keeps
  // composing with a server-side candidate window. Commit whenever Rime is
  // composing, even without a live TSF composition.
  if (!_status.composing && !_IsComposing()) {
    _cand->Destroy();
    return;
  }
  // The framework may Deactivate (EndSession) before focus-loss
  // notifications arrive. With a dead session the server has already
  // destroyed the Rime state: sending Commit would read stale pipe data,
  // so only clean up local state here. Deactivate() commits first while
  // the session is still alive, covering that order.
  if (!m_client.IsActive()) {
    DEBUG << "_CommitComposition: session dead, local cleanup only";
    if (_IsComposing()) {
      if (_pEditSessionContext)
        _EndComposition(_pEditSessionContext, true);
      _FinalizeComposition();
    }
    _committed = TRUE;
    // Thread manager may already be torn down here: EndUI() would no-op,
    // so Destroy() (now flag-safe) to hide the orphan window.
    _cand->Destroy();
    return;
  }
  // Commit the Rime-side composition first, so in-flight input is not lost
  // on language switch / focus loss. The server streams the commit text
  // back through the IPC channel (see OnCommitComposition).
  m_client.CommitComposition();
  std::wstring commit;
  weasel::ResponseParser parser(&commit, NULL, &_status, NULL,
                                &_cand->style());
  bool gotResponse = m_client.GetResponseData(std::ref(parser));
  DEBUG << "_CommitComposition: response=" << gotResponse
        << " commit_len=" << commit.length();
  if (!commit.empty())
    DEBUG << "_CommitComposition: commit=" << commit;
  if (!gotResponse)
    commit.clear();
  _UpdateLanguageBar(_status);

  com_ptr<ITfContext> pContext = _pEditSessionContext;
  com_ptr<ITfComposition> pComposition = _pComposition;
  // NOTE: intentionally Destroy(), not EndUI(): EndUIElement() issued from
  // inside Deactivate/focus-loss teardown was observed to intermittently
  // stall profile switches. Destroy() is flag-safe since the
  // CCandidateList::Destroy fix (resets _uiStarted).
  _cand->Destroy();
  _committed = TRUE;
  if (pComposition && pContext) {
    com_ptr<CCommitCompositionEditSession> pEditSession;
    pEditSession.Attach(new CCommitCompositionEditSession(
        this, pContext, pComposition, commit));
    if (pEditSession != nullptr) {
      // Synchronous: the TSF composition must be fully terminated before
      // this focus-loss notification returns, otherwise the pending
      // language switch is cancelled and the user has to press the hotkey
      // a second time. If the app downgrades to async (TF_S_ASYNC), the
      // session still runs later -- no worse than the old behavior.
      HRESULT hrSession = S_OK;
      HRESULT hrReq = pContext->RequestEditSession(
          _tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
      DEBUG << "_CommitComposition: RequestEditSession req=" << hrReq
            << " session=" << hrSession;
    }
  } else if (!commit.empty() && pContext) {
    // Rime was composing without a live TSF composition (host-terminated):
    // insert the committed text at the caret, no composition to end.
    com_ptr<CCommitTextEditSession> pEditSession;
    pEditSession.Attach(new CCommitTextEditSession(this, pContext, commit));
    if (pEditSession != nullptr) {
      HRESULT hrSession = S_OK;
      HRESULT hrReq = pContext->RequestEditSession(
          _tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
      DEBUG << "_CommitComposition: insert-only req=" << hrReq
            << " session=" << hrSession;
    }
  } else {
    DEBUG << "_CommitComposition: nothing to insert, UI destroyed";
  }
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}

BOOL WeaselTSF::_IsCurrentComposition(ITfComposition* pComposition) {
  return _pComposition != nullptr && _pComposition == pComposition;
}
