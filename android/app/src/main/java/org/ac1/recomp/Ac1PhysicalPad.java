package org.ac1.recomp;

import android.content.Context;
import android.hardware.input.InputManager;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/** One active physical controller at XInput user 0. Hot-plug chooses the first
 * eligible device, retaining it until removal; a second pad can take its place. */
final class Ac1PhysicalPad implements InputManager.InputDeviceListener {
    private final InputManager manager;
    private final Ac1ControllerView.Sink sink;
    private final Runnable changed;
    private int deviceId=-1, buttons, hat, digitalLT, digitalRT;
    private float lx,ly,rx,ry,lt,rt;
    private boolean running;
    Ac1PhysicalPad(Context context, Ac1ControllerView.Sink sink, Runnable changed) {
        manager=(InputManager)context.getSystemService(Context.INPUT_SERVICE);
        this.sink=sink;this.changed=changed;manager.registerInputDeviceListener(this,null);scan();
    }
    boolean connected(){return deviceId!=-1;}
    private static boolean eligible(InputDevice d) {
        if(d==null||d.isVirtual())return false;
        return d.supportsSource(InputDevice.SOURCE_GAMEPAD)||d.supportsSource(InputDevice.SOURCE_JOYSTICK);
    }
    private void scan(){
        if(eligible(manager.getInputDevice(deviceId)))return;
        deviceId=-1;
        for(int id:manager.getInputDeviceIds())if(eligible(manager.getInputDevice(id))){deviceId=id;break;}
        neutral();changed.run();publish();
    }
    void setRunning(boolean value){running=value;neutral();publish();}
    private void neutral(){buttons=hat=digitalLT=digitalRT=0;lx=ly=rx=ry=lt=rt=0;}
    void close(){manager.unregisterInputDeviceListener(this);}
    @Override public void onInputDeviceAdded(int id){scan();}
    @Override public void onInputDeviceRemoved(int id){scan();}
    @Override public void onInputDeviceChanged(int id){if(id==deviceId){deviceId=-1;scan();}else scan();}
    private void publish(){if(connected())sink.state(running,buttons|hat,Math.max(digitalLT,trigger(lt)),Math.max(digitalRT,trigger(rt)),stick(lx),stick(-ly),stick(rx),stick(-ry));}
    private static int stick(float v){v=Math.max(-1,Math.min(1,v));return Math.round(v*(v<0?32768:32767));}
    private static int trigger(float v){return Math.round(Math.max(0,Math.min(1,v))*255);}
    boolean key(KeyEvent e){
        if(!running||e.getDeviceId()!=deviceId||!connected())return false;
        if(e.getAction()!=KeyEvent.ACTION_DOWN&&e.getAction()!=KeyEvent.ACTION_UP)return false;
        int bit=0;boolean down=e.getAction()==KeyEvent.ACTION_DOWN;
        switch(e.getKeyCode()){
        case KeyEvent.KEYCODE_BUTTON_A:bit=4096;break;case KeyEvent.KEYCODE_BUTTON_B:bit=8192;break;
        case KeyEvent.KEYCODE_BUTTON_X:bit=16384;break;case KeyEvent.KEYCODE_BUTTON_Y:bit=32768;break;
        case KeyEvent.KEYCODE_BUTTON_L1:bit=256;break;case KeyEvent.KEYCODE_BUTTON_R1:bit=512;break;
        case KeyEvent.KEYCODE_BUTTON_THUMBL:bit=64;break;case KeyEvent.KEYCODE_BUTTON_THUMBR:bit=128;break;
        case KeyEvent.KEYCODE_BUTTON_START:bit=16;break;case KeyEvent.KEYCODE_BUTTON_SELECT:case KeyEvent.KEYCODE_BACK:bit=32;break;
        case KeyEvent.KEYCODE_DPAD_UP:bit=1;break;case KeyEvent.KEYCODE_DPAD_DOWN:bit=2;break;
        case KeyEvent.KEYCODE_DPAD_LEFT:bit=4;break;case KeyEvent.KEYCODE_DPAD_RIGHT:bit=8;break;
        case KeyEvent.KEYCODE_BUTTON_L2:digitalLT=down?255:0;break;
        case KeyEvent.KEYCODE_BUTTON_R2:digitalRT=down?255:0;break;
        default:return false;
        }
        if(down)buttons|=bit;else buttons&=~bit;publish();return true;
    }
    private static boolean has(InputDevice d,int axis){return d.getMotionRange(axis,InputDevice.SOURCE_JOYSTICK)!=null||d.getMotionRange(axis)!=null;}
    private static float axis(MotionEvent e,InputDevice d,int axis,boolean trigger){
        InputDevice.MotionRange r=d.getMotionRange(axis,e.getSource());if(r==null)r=d.getMotionRange(axis);
        if(r==null)return 0;
        float v=e.getAxisValue(axis),min=r.getMin(),max=r.getMax();
        if(max<=min)return 0;
        // Do not apply MotionRange.flat: AC1 owns its deadzones. Normalize the
        // full advertised range, including trigger drivers that report -1..1.
        return trigger?(v-min)/(max-min):((v-min)/(max-min))*2-1;
    }
    boolean motion(MotionEvent e){
        if(!running||e.getDeviceId()!=deviceId||!connected()||!e.isFromSource(InputDevice.SOURCE_JOYSTICK))return false;
        InputDevice d=e.getDevice();if(d==null)return false;
        lx=axis(e,d,MotionEvent.AXIS_X,false);ly=axis(e,d,MotionEvent.AXIS_Y,false);
        rx=axis(e,d,has(d,MotionEvent.AXIS_Z)?MotionEvent.AXIS_Z:MotionEvent.AXIS_RX,false);
        ry=axis(e,d,has(d,MotionEvent.AXIS_RZ)?MotionEvent.AXIS_RZ:MotionEvent.AXIS_RY,false);
        lt=axis(e,d,has(d,MotionEvent.AXIS_LTRIGGER)?MotionEvent.AXIS_LTRIGGER:MotionEvent.AXIS_BRAKE,true);
        rt=axis(e,d,has(d,MotionEvent.AXIS_RTRIGGER)?MotionEvent.AXIS_RTRIGGER:MotionEvent.AXIS_GAS,true);
        float hx=e.getAxisValue(MotionEvent.AXIS_HAT_X),hy=e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hat=(hx<-.5?4:hx>.5?8:0)|(hy<-.5?1:hy>.5?2:0);publish();return true;
    }
}
