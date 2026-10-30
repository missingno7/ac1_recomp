package org.ac1.recomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.Log;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.DisplayCutout;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;

/** AC1's contextual puppeteering pad. Coordinates are fractions of the safe area;
 * radii are fractions of its shorter dimension. The SurfaceView stays underneath. */
final class Ac1ControllerView extends View {
    interface Sink { void state(boolean connected, int buttons, int lt, int rt, int lx, int ly, int rx, int ry); }
    private final Sink sink;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF safe = new RectF();
    private final SparseArray<Finger> fingers = new SparseArray<>();
    private final ArrayList<Control> controls = new ArrayList<>(), weapons = new ArrayList<>();
    private float opacity, sensitivity, leftInset, topInset, rightInset, bottomInset;
    private RectF camera;
    private boolean active, hidden, popup, rtLocked;
    private float moveX, moveY, originX, originY, cameraX, cameraY;
    private static final class Control {
        String id, label; float x, y, r;
        Control(JSONObject j) throws Exception {
            id=j.getString("id"); label=j.getString("label");
            x=bounded(j.getDouble("x"),0,1); y=bounded(j.getDouble("y"),0,1);
            r=bounded(j.getDouble("r"),0.015f,0.25f);
            if (!id.equals("move") && !id.equals("weapons") && !id.equals("hide") &&
                !id.equals("rtLock") && !id.equals("LT") && !id.equals("RT") && bit(id)==0)
                throw new IllegalArgumentException("Unknown controller control: "+id);
        }
    }
    private static final class Finger {
        String role; float x,y; long time;
        Finger(String role,float x,float y,long time) { this.role=role;this.x=x;this.y=y;this.time=time; }
    }
    private static float bounded(double value,float min,float max) {
        if (!Double.isFinite(value) || value<min || value>max) throw new IllegalArgumentException("Invalid controller coordinate/setting");
        return (float)value;
    }
    Ac1ControllerView(Context context, Sink sink) {
        super(context); this.sink=sink; setFocusable(false);
        try {
            byte[] defaults;
            try (java.io.InputStream in=context.getAssets().open("controller-layout.json")) { java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream(); byte[] buffer=new byte[4096]; for(int n;(n=in.read(buffer))!=-1;)out.write(buffer,0,n); defaults=out.toByteArray(); }
            File file=new File(context.getFilesDir(),"controller-layout.json");
            if (!file.exists()) Files.write(file.toPath(),defaults);
            try { load(new JSONObject(new String(Files.readAllBytes(file.toPath()),StandardCharsets.UTF_8))); }
            catch (Exception invalid) {
                Log.e("AC1", "Invalid controller-layout.json; using packaged defaults",invalid);
                load(new JSONObject(new String(defaults,StandardCharsets.UTF_8)));
            }
        } catch (Exception error) { throw new IllegalStateException("Cannot load AC1 controller layout",error); }
        hidden=context.getSharedPreferences("controller",0).getBoolean("hidden",false);
        setVisibility(GONE);
        setOnApplyWindowInsetsListener((v,insets)-> {
            leftInset=insets.getSystemWindowInsetLeft(); topInset=insets.getSystemWindowInsetTop();
            rightInset=insets.getSystemWindowInsetRight(); bottomInset=insets.getSystemWindowInsetBottom();
            DisplayCutout cutout=insets.getDisplayCutout();
            if(cutout!=null) {
                leftInset=Math.max(leftInset,cutout.getSafeInsetLeft()); topInset=Math.max(topInset,cutout.getSafeInsetTop());
                rightInset=Math.max(rightInset,cutout.getSafeInsetRight()); bottomInset=Math.max(bottomInset,cutout.getSafeInsetBottom());
            }
            if(android.os.Build.VERSION.SDK_INT>=30) {
                android.graphics.Insets gestures=insets.getInsets(WindowInsets.Type.mandatorySystemGestures());
                leftInset=Math.max(leftInset,gestures.left);rightInset=Math.max(rightInset,gestures.right);
                topInset=Math.max(topInset,gestures.top);bottomInset=Math.max(bottomInset,gestures.bottom);
            }
            updateSafe();return insets;
        });
    }
    private void load(JSONObject j) throws Exception {
        if(j.getInt("version")!=1) throw new IllegalArgumentException("Unsupported controller layout version");
        controls.clear(); weapons.clear();
        opacity=bounded(j.getDouble("opacity"),0.1f,1); sensitivity=bounded(j.getDouble("cameraSensitivity"),0.01f,5);
        JSONArray list=j.getJSONArray("controls");for(int i=0;i<list.length();++i)controls.add(new Control(list.getJSONObject(i)));
        list=j.getJSONArray("weapons");for(int i=0;i<list.length();++i)weapons.add(new Control(list.getJSONObject(i)));
        JSONObject area=j.getJSONObject("camera");
        camera=new RectF(bounded(area.getDouble("left"),0,1),bounded(area.getDouble("top"),0,1),
            bounded(area.getDouble("right"),0,1),bounded(area.getDouble("bottom"),0,1));
        if(camera.isEmpty())throw new IllegalArgumentException("Empty camera region");
    }
    void setActive(boolean enabled) {
        clear(); active=enabled; setVisibility(enabled?VISIBLE:GONE); publish(); if(enabled) requestApplyInsets();
    }
    void clear() {
        fingers.clear(); moveX=moveY=cameraX=cameraY=0; rtLocked=false; popup=false;
        removeCallbacks(cameraIdle); if(active)publish(); invalidate();
    }
    private void updateSafe() {
        float margin=8*getResources().getDisplayMetrics().density;
        safe.set(leftInset+margin,topInset+margin,getWidth()-rightInset-margin,getHeight()-bottomInset-margin);invalidate();
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){updateSafe();clear();}
    private float unit(){return Math.min(safe.width(),safe.height());}
    private float x(Control c){return safe.left+c.x*safe.width();}
    private float y(Control c){return safe.top+c.y*safe.height();}
    private Control hit(float x,float y,ArrayList<Control> list){
        for(int i=list.size()-1;i>=0;--i){Control c=list.get(i);if(Math.hypot(x-x(c),y-y(c))<=c.r*unit())return c;}return null;
    }
    private boolean owned(String role){for(int i=0;i<fingers.size();i++)if(fingers.valueAt(i).role.equals(role))return true;return false;}
    static int bit(String id) {
        switch(id){case "UP":return 1;case "DOWN":return 2;case "LEFT":return 4;case "RIGHT":return 8;
        case "START":return 16;case "BACK":return 32;case "LS":return 64;case "RS":return 128;
        case "LB":return 256;case "RB":return 512;case "A":return 4096;case "B":return 8192;
        case "X":return 16384;case "Y":return 32768;default:return 0;}
    }
    private void publish(){
        int buttons=0,lt=0,rt=rtLocked?255:0;
        if(!hidden)for(int i=0;i<fingers.size();i++){String r=fingers.valueAt(i).role;buttons|=bit(r);if(r.equals("LT"))lt=255;if(r.equals("RT"))rt=255;}
        sink.state(active&&!hidden,buttons,lt,rt,raw(moveX),raw(-moveY),raw(cameraX),raw(-cameraY));invalidate();
    }
    private static int raw(float v){v=Math.max(-1,Math.min(1,v));return Math.round(v*(v<0?32768:32767));}
    private final Runnable cameraIdle=()->{cameraX=cameraY=0;publish();};
    @Override public boolean onTouchEvent(MotionEvent e){
        if(!active)return false;
        int action=e.getActionMasked(),index=e.getActionIndex(),id=e.getPointerId(index);
        float px=e.getX(index),py=e.getY(index);
        if(action==MotionEvent.ACTION_CANCEL){clear();return true;}
        if(action==MotionEvent.ACTION_DOWN||action==MotionEvent.ACTION_POINTER_DOWN){
            Control c=popup?hit(px,py,weapons):null;if(c==null)c=hit(px,py,controls);
            if(hidden){if(c!=null&&c.id.equals("hide"))toggleHidden();return true;}
            if(c!=null&&c.id.equals("hide")){toggleHidden();return true;}
            if(c!=null&&c.id.equals("rtLock")){rtLocked=!rtLocked;publish();return true;}
            if(c!=null&&c.id.equals("weapons")){popup=!popup;invalidate();return true;}
            String role=c==null?"":c.id;
            if(c==null&&safe.contains(px,py)&&px<safe.left+safe.width()*0.45f&&py>safe.top+safe.height()*0.48f)role="move";
            if(c==null&&role.isEmpty()&&camera.contains((px-safe.left)/safe.width(),(py-safe.top)/safe.height()))role="camera";
            if(role.isEmpty()||owned(role))return true;
            fingers.put(id,new Finger(role,px,py,e.getEventTime()));
            if(role.equals("move")){float radius=unit()*0.125f;for(Control control:controls)if(control.id.equals("move"))radius=unit()*control.r;
                originX=Math.max(safe.left+radius,Math.min(safe.right-radius,px));originY=Math.max(safe.top+radius,Math.min(safe.bottom-radius,py));moveX=moveY=0;}
        }else if(action==MotionEvent.ACTION_MOVE){
            for(int i=0;i<e.getPointerCount();i++){
                Finger f=fingers.get(e.getPointerId(i));if(f==null)continue;
                px=e.getX(i);py=e.getY(i);
                if(f.role.equals("move")){
                    float radius=unit()*0.125f;for(Control c:controls)if(c.id.equals("move"))radius=unit()*c.r;
                    moveX=(px-originX)/radius;moveY=(py-originY)/radius;
                    float length=(float)Math.hypot(moveX,moveY);if(length>1){moveX/=length;moveY/=length;}
                }else if(f.role.equals("camera")){
                    float scale=sensitivity*1000f/(Math.max(1,e.getEventTime()-f.time)*unit());
                    cameraX=(px-f.x)*scale;cameraY=(py-f.y)*scale;
                    removeCallbacks(cameraIdle);postDelayed(cameraIdle,80);
                }
                f.x=px;f.y=py;f.time=e.getEventTime();
            }
        }else if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP){
            Finger f=fingers.get(id);fingers.remove(id);
            if(f!=null&&f.role.equals("move"))moveX=moveY=0;
            if(f!=null&&f.role.equals("camera")){cameraX=cameraY=0;removeCallbacks(cameraIdle);}
        }
        publish();return true;
    }
    private void toggleHidden(){clear();hidden=!hidden;getContext().getSharedPreferences("controller",0).edit().putBoolean("hidden",hidden).apply();publish();}
    @Override protected void onDraw(Canvas canvas){
        super.onDraw(canvas);if(!active||safe.isEmpty())return;
        paint.setTextAlign(Paint.Align.CENTER);paint.setTypeface(android.graphics.Typeface.create("sans-serif-medium",0));
        for(Control c:controls)if(!hidden||c.id.equals("hide"))drawControl(canvas,c);
        if(!hidden&&popup)for(Control c:weapons)drawControl(canvas,c);
        if(!hidden){
            paint.setColor(Color.argb((int)(opacity*160),225,235,242));paint.setTextSize(unit()*0.022f);
            canvas.drawText("Swipe to look",safe.left+safe.width()*0.59f,safe.top+safe.height()*0.59f,paint);
        }
    }
    private void drawControl(Canvas canvas,Control c){
        if(popup&&c.id.equals("weapons"))return;
        float cx=x(c),cy=y(c),radius=c.r*unit();boolean pressed=owned(c.id)||(c.id.equals("rtLock")&&rtLocked);
        if(c.id.equals("move")&&owned("move")){cx=originX;cy=originY;}
        paint.setStyle(Paint.Style.FILL);paint.setColor(Color.argb((int)(opacity*190),pressed?58:20,pressed?113:30,pressed?129:39));canvas.drawCircle(cx,cy,radius,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(unit()*0.0015f);paint.setColor(Color.argb((int)(opacity*255),210,225,235));canvas.drawCircle(cx,cy,radius,paint);paint.setStyle(Paint.Style.FILL);
        if(c.id.equals("move")){canvas.drawCircle(cx+moveX*radius,cy+moveY*radius,radius*0.33f,paint);}
        else{
            paint.setTextSize(unit()*(c.r<0.04?0.019f:0.025f));
            String symbol=c.id.equals("hide")?(hidden?"Show pad":"Hide pad"):c.label;
            if(paint.measureText(symbol)>radius*1.75f)paint.setTextSize(paint.getTextSize()*radius*1.75f/paint.measureText(symbol));
            canvas.drawText(symbol,cx,cy+paint.getTextSize()*0.34f,paint);
            if("ABXY".contains(c.id)&&c.id.length()==1){
                paint.setTextSize(unit()*0.025f);canvas.drawText(c.id,cx,cy-radius*0.42f,paint);
            }
        }
    }
}
