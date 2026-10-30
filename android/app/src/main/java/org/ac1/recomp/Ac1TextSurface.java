package org.ac1.recomp;

import android.content.Context;
import android.text.Editable;
import android.text.InputType;
import android.text.Selection;
import android.text.SpannableStringBuilder;
import android.view.KeyEvent;
import android.view.SurfaceView;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

/** IME composition stays local. Only committed UTF-16 reaches AC1's original
 * WM_KEYDOWN -> ToUnicode -> filtered text queue. No editable guest buffer is
 * exposed to the IME and no guessed game-state address is polled. */
final class Ac1TextSurface extends SurfaceView {
    interface Sink {
        int commit(String text);
        int key(int vk);
    }
    private final Sink sink;
    private final Runnable changed;
    private boolean editing;
    private int generation;
    private Connection connection;
    private final Editable contextText=new SpannableStringBuilder();

    Ac1TextSurface(Context context, Sink sink, Runnable changed) {
        super(context); this.sink=sink; this.changed=changed;
        setFocusable(true); setFocusableInTouchMode(true);
    }
    boolean isEditing() { return editing; }
    void showKeyboard() {
        editing=true; ++generation; connection=null;
        requestFocus(); changed.run();
        InputMethodManager ime=(InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        ime.restartInput(this);
        post(()-> { if(editing)ime.showSoftInput(this,InputMethodManager.SHOW_IMPLICIT); });
    }
    void hideKeyboard(boolean escape) {
        if(!editing)return;
        if(!escape && connection!=null)connection.finishComposingText();
        editing=false; ++generation; connection=null;
        InputMethodManager ime=(InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        ime.hideSoftInputFromWindow(getWindowToken(),0);
        if(escape)deliverKey(27);
        changed.run();
    }
    void resetEditor() {
        // Lifecycle teardown must not enqueue a half-composed name for the
        // next guest run after its input session has already shut down.
        editing=false; ++generation; connection=null;contextText.clear();
        ((InputMethodManager)getContext().getSystemService(Context.INPUT_METHOD_SERVICE))
            .hideSoftInputFromWindow(getWindowToken(),0);
        changed.run();
    }
    private boolean deliver(String text) {
        int result=sink.commit(text);
        if(result!=0)Toast.makeText(getContext(),result==-1
            ? "AC1 names support Western Latin letters, numbers, spaces, + and -. Unsupported text was not sent."
            : "AC1 text input is busy or replaying. Text was not sent.",Toast.LENGTH_LONG).show();
        return result==0;
    }
    private boolean deliverKey(int vk) {
        int result=sink.key(vk);
        if(result!=0)Toast.makeText(getContext(),"AC1 text input is busy or replaying.",Toast.LENGTH_SHORT).show();
        return result==0;
    }
    @Override public boolean onCheckIsTextEditor() { return editing; }
    @Override public InputConnection onCreateInputConnection(EditorInfo info) {
        if(!editing)return null;
        info.inputType=InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        info.imeOptions=EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        info.initialSelStart=info.initialSelEnd=contextText.length();
        connection=new Connection(generation); return connection;
    }
    @Override public boolean onKeyPreIme(int code,KeyEvent event) {
        if(editing && code==KeyEvent.KEYCODE_BACK) {
            if(event.getAction()==KeyEvent.ACTION_UP)hideKeyboard(true);
            return true;
        }
        return super.onKeyPreIme(code,event);
    }
    private final class Connection extends BaseInputConnection {
        private final int epoch;
        private final Editable composing=contextText;
        Connection(int epoch) { super(Ac1TextSurface.this,true);this.epoch=epoch;Selection.setSelection(composing,composing.length()); }
        private boolean valid() { return editing && epoch==generation; }
        @Override public Editable getEditable() { return composing; }
        @Override public boolean setSelection(int start,int end) {
            // The game owns its caret. Do not advertise arbitrary selection or
            // replacement of text the IME has not read from the guest.
            return valid() && start==end && end==composing.length() && super.setSelection(start,end);
        }
        private void clear() { composing.clear();Selection.setSelection(composing,0); }
        @Override public boolean setComposingText(CharSequence text,int cursor) {
            return valid() && super.setComposingText(text,cursor);
        }
        @Override public boolean commitText(CharSequence text,int cursor) {
            if(!valid())return false;
            boolean ok=deliver(text.toString());
            // A rejected commit is also removed from the IME buffer; it must
            // not reappear as a stale composition on the next commit.
            if(ok)super.commitText(text,cursor);
            else { super.finishComposingText(); clear(); }
            return ok;
        }
        @Override public boolean finishComposingText() {
            if(!valid())return false;
            int start=getComposingSpanStart(composing),end=getComposingSpanEnd(composing);
            boolean ok=start<0 || end<=start || deliver(composing.subSequence(start,end).toString());
            super.finishComposingText();
            if(!ok)clear();
            return ok;
        }
        @Override public boolean deleteSurroundingText(int before,int after) {
            if(!valid() || before<0 || after!=0 || before>64)return false;
            if(getComposingSpanStart(composing)>=0) {
                int cursor=Math.max(0,Selection.getSelectionEnd(composing));
                int start=Math.max(getComposingSpanStart(composing),cursor-before);
                composing.delete(start,cursor);Selection.setSelection(composing,start);return true;
            }
            for(int i=0;i<before;i++)if(!deliverKey(8))return false;
            int cursor=Math.max(0,Selection.getSelectionEnd(composing));
            int start=Math.max(0,cursor-before);
            composing.delete(start,cursor);Selection.setSelection(composing,start);return true;
        }
        @Override public boolean deleteSurroundingTextInCodePoints(int before,int after) {
            return deleteSurroundingText(before,after); // accepted game characters are all BMP
        }
        @Override public boolean performEditorAction(int action) {
            if(!valid())return false;
            if(!finishComposingText())return false;
            if(!deliverKey(13))return false;
            hideKeyboard(false);return true;
        }
        @Override public boolean sendKeyEvent(KeyEvent event) {
            if(!valid())return false;
            int code=event.getKeyCode();
            if(code==KeyEvent.KEYCODE_ENTER) {
                return event.getAction()!=KeyEvent.ACTION_DOWN || performEditorAction(EditorInfo.IME_ACTION_DONE);
            }
            if(code==KeyEvent.KEYCODE_DEL) {
                return event.getAction()!=KeyEvent.ACTION_DOWN || deleteSurroundingText(1,0);
            }
            if(code==KeyEvent.KEYCODE_ESCAPE || code==KeyEvent.KEYCODE_BACK) {
                if(event.getAction()==KeyEvent.ACTION_UP)hideKeyboard(true);
                return true;
            }
            if(event.getAction()==KeyEvent.ACTION_UP)return true;
            int cp=event.getUnicodeChar();
            return cp>0 && deliver(new String(Character.toChars(cp)));
        }
    }
}
