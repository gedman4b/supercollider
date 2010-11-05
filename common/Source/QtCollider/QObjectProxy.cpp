/************************************************************************
*
* Copyright 2010 Jakob Leben (jakob.leben@gmail.com)
*
* This file is part of SuperCollider Qt GUI.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
************************************************************************/

#include "QObjectProxy.h"
#include "QcApplication.h"
#include "Common.h"
#include "Slot.h"
#include "QcSignalSpy.h"

#include <QApplication>
#include <QWidget>
#include <QVarLengthArray>

#include <PyrKernel.h>
#include <VMGlobals.h>

using namespace QtCollider;

void interpretMouseEvent( QEvent *e, QList<QVariant> &args );
void interpretKeyEvent( QEvent *e, QList<QVariant> &args );

QObjectProxy::QObjectProxy( QObject *qObject_, PyrObject *scObject_ )
: QObject( qObject_ ),
  qObject( qObject_ ),
  scObject( scObject_ ),
  sigSpy( new QcSignalSpy( this ) )
{
  qObject->installEventFilter( this );
}

QObjectProxy::~QObjectProxy()
{
  if( scObject ) {
    qcProxyDebugMsg( 2, "~QObjectProxy: invalidating SC object" );
    QtCollider::lockLang();
    SetNil( scObject->slots );
    QtCollider::unlockLang();
  }
  else {
    qcProxyDebugMsg( 2, "~QObjectProxy: SC object already detached" );
  }
}

const char *QObjectProxy::scClassName() const {
  if( scObject ) return slotRawSymbol( &scObject->classptr->name )->name;
  return 0;
}

bool QObjectProxy::invokeMethod( const char *method, PyrSlot *retSlot, PyrSlot *argSlot,
                                 Qt::ConnectionType ctype )
{
  // the signature char array
  QVarLengthArray<char, 512> sig;

  // serialize method name
  int len = qstrlen( method );
  if( len <= 0 ) return false;
  sig.append( method, len );
  sig.append( '(' );

  // get data from argument slots
  Slot argSlots[10];

  if( isKindOfSlot( argSlot, class_array ) ) {
    PyrSlot *slots = slotRawObject( argSlot )->slots;
    int size = slotRawObject( argSlot )->size;
    int i;
    for( i = 0; i<size && i<10; ++i ) {
      argSlots[i].setData( slots );
      ++slots;
    }
  }
  else argSlots[0].setData( argSlot );

  // serialize argument types
  int i;
  for( i = 0; i < 10; ++i ) {
    int type = argSlots[i].type();
    if( type == QMetaType::Void ) break;
    const char *typeName = QMetaType::typeName( type );
    int len = qstrlen( typeName );
    if( len <= 0 ) break;
    sig.append( typeName, len );
    sig.append( ',' );
  }

  // finalize the signature
  if( i==0 ) sig.append( ')' );
  else sig[sig.size() - 1] = ')';

  sig.append('\0');

  // get the meta method
  const QMetaObject *mo = qObject->metaObject();

  int mi = mo->indexOfMethod( sig.constData() );
  if( mi < 0 ) {
    QByteArray mnorm = QMetaObject::normalizedSignature( sig.constData() );
    mi = mo->indexOfMethod( mnorm.constData() );
  }

  if( mi < 0 || mi >= mo->methodCount()  ) {
    qcProxyDebugMsg( 1, QString("WARNING: No such method: %1::%2").arg( mo->className() )
                        .arg( sig.constData() ) );
    return false;
  }

  QMetaMethod mm = mo->method( mi );

  // construct the return data object
  QGenericReturnArgument retGArg;
  const char *retTypeName = mm.typeName();
  int retType = QMetaType::type( retTypeName );
  void *retPtr = 0;
  if( retSlot ) {
    retPtr = QMetaType::construct( retType );
    retGArg = QGenericReturnArgument( retTypeName, retPtr );
  }

  //do it!
  bool success =
    mm.invoke( qObject, ctype, retGArg,
                argSlots[0].asGenericArgument(),
                argSlots[1].asGenericArgument(),
                argSlots[2].asGenericArgument(),
                argSlots[3].asGenericArgument(),
                argSlots[4].asGenericArgument(),
                argSlots[5].asGenericArgument(),
                argSlots[6].asGenericArgument(),
                argSlots[7].asGenericArgument(),
                argSlots[8].asGenericArgument(),
                argSlots[9].asGenericArgument());

  // store the return data into the return slot
  if( success && retPtr ) {
    QVariant retVar( retType, retPtr );
    Slot::setVariant( retSlot, retVar );
  };

  if( retPtr )
    QMetaType::destroy( retType, retPtr );

  return success;
}

void QObjectProxy::invokeScMethod
( PyrSymbol *method, const QList<QVariant> & args, PyrSlot *result,
  bool locked )
{
  qcProxyDebugMsg(1, QString("+++ SC METHOD CALL: ") + QString(method->name) );

  if( !locked ) {
    QtCollider::lockLang();
  }

  if( scObject ) {
    VMGlobals *g = gMainVMGlobals;
    g->canCallOS = true;
    ++g->sp;  SetObject(g->sp, scObject);
    Q_FOREACH( QVariant var, args ) {
      ++g->sp;
      if( Slot::setVariant( g->sp, var ) )
        SetNil( g->sp );
    }
    runInterpreter(g, method, args.size() + 1);
    g->canCallOS = false;
    if (result) slotCopy(result, &g->result);
  }
  else {
    SetNil( result );
    qcDebugMsg(1, "WARNING: no SC object");
  }

  if( !locked ) QtCollider::unlockLang();

  qcProxyDebugMsg(1, QString("--- SC METHOD CALL: ") + QString(method->name) );
}

void QObjectProxy::customEvent( QEvent *event )
{
  if( event->type() == (QEvent::Type) QtCollider::Event_ScMethodCall ) {
    scMethodCallEvent( static_cast<ScMethodCallEvent*>( event ) );
    return;
  }

  if( event->type() != (QEvent::Type) QtCollider::Event_Sync ) return;

  QcSyncEvent *se = static_cast<QcSyncEvent*>( event );

  if( se->syncEventType() == QcSyncEvent::ProxyRequest ) {
    QtCollider::RequestEvent *re =  static_cast<QtCollider::RequestEvent*>( event );
    re->execute( this );
    return;
  }
}

bool QObjectProxy::setParentEvent( SetParentEvent *e ) {
  qObject->setParent( e->parent );
  return true;
}

bool QObjectProxy::setPropertyEvent( SetPropertyEvent *e )
{
  if( !qObject->setProperty( e->property->name, e->value ) ) {
    qcProxyDebugMsg(1, QString("WARNING: Property '%1' not found. Setting dynamic property.")
                        .arg( e->property->name ) );
  }
}

bool QObjectProxy::getPropertyEvent( GetPropertyEvent *e )
{
  e->value = qObject->property( e->property->name );
  return true;
}

bool QObjectProxy::setEventHandlerEvent( SetEventHandlerEvent *e )
{
  EventHandlerData data;
  data.type = e->type;
  data.method = e->method;
  data.sync = e->sync;
  switch( e->type ) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Enter:
      data.interpretFn = &QObjectProxy::interpretMouseEvent; break;
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
      data.interpretFn = &QObjectProxy::interpretKeyEvent; break;
    default:
      data.interpretFn = 0;
  }
  eventHandlers.insert( data.type, data );
  return true;
}

bool QObjectProxy::connectEvent( ConnectEvent *e )
{
  bool direct = e->sync == Synchronous;
  return sigSpy->connect( e->signal.toStdString().c_str(), e->handler, direct );
}

bool QObjectProxy::invokeMethodEvent( InvokeMethodEvent *e )
{
  return invokeMethod( e->method->name, e->ret, e->arg, Qt::DirectConnection );
}

bool QObjectProxy::destroyEvent( DestroyEvent *e )
{
  scObject = 0;

  if( e->action() == DestroyProxyAndObject )
    qObject->deleteLater();
  else
    deleteLater();

  return errNone;
}

bool QObjectProxy::eventFilter( QObject * watched, QEvent * event )
{
  int type = event->type();

  if( type == QtCollider::Event_ScMethodCall ) {
    ScMethodCallEvent* mce = static_cast<ScMethodCallEvent*>( event );
    qcProxyDebugMsg(1, QString("ScMethodCallEvent -> ") + QString(mce->method->name ) );
    scMethodCallEvent( mce );
    return true;
  }
  else {
    EventHandlerData eh = eventHandlers.value( type, EventHandlerData() );
    if( eh.type == type ) {
      PyrSymbol *symMethod = eh.method;
      qcProxyDebugMsg(1,QString("Catched event: type %1 -> '%2'").arg(type).arg(symMethod->name) );

      InterpretEventFn interpreter = eh.interpretFn;

      QList<QVariant> args;

      if( interpreter ) {
        qcProxyDebugMsg(2,"got interpreter");
        (this->*interpreter) ( event, args );
      }

      if( eh.sync == Synchronous ) {
        qcProxyDebugMsg(2,"direct!");
        PyrSlot result;
        invokeScMethod( symMethod, args, &result );
        if( IsNil( &result ) ) return false;
        else {
          if( IsFalse( &result ) ) event->ignore();
          return true;
        }
      }
      else {
        qcProxyDebugMsg(2,"indirect");
        ScMethodCallEvent *e = new ScMethodCallEvent( symMethod, args );
        QApplication::postEvent( this, e );
      }
    }
    return false;
  }
}


void QObjectProxy::scMethodCallEvent( ScMethodCallEvent *e )
{
  invokeScMethod( e->method, e->args, 0, e->locked );
}

#include <QMouseEvent>

void QObjectProxy::interpretMouseEvent( QEvent *e, QList<QVariant> &args )
{
  if( e->type() == QEvent::Enter ) {
    QPoint pos = QCursor::pos();

    QWidget *w = qobject_cast<QWidget*>( qObject );
    if( w ) pos = w->mapFromGlobal( pos );

    args << pos.x();
    args << pos.y();
    return;
  }

  QMouseEvent *mouse = static_cast<QMouseEvent*>( e );
  args << mouse->x();
  args << mouse->y();

  args << (int) mouse->modifiers();

  if( e->type() == QEvent::MouseMove ) return;

  int button;
  switch( mouse->button() ) {
    case Qt::LeftButton:
      button = 0; break;
    case Qt::RightButton:
      button = 1; break;
    case Qt::MidButton:
      button = 2; break;
    default:
      button = -1;
  }

  args << button;

  switch( e->type() ) {
    case QEvent::MouseButtonPress:
      args << 1; break;
    case QEvent::MouseButtonDblClick:
      args << 2; break;
    default: ;
  }
}

void QObjectProxy::interpretKeyEvent( QEvent *e, QList<QVariant> &args )
{
  QKeyEvent *ke = static_cast<QKeyEvent*>( e );

  QString text = ke->text();
  int unicode = ( text.count() == 1 ? text[0].unicode() : 0 );

  args << text;
  args << (int) ke->modifiers();
  args << unicode;
  args << ke->key();
}

bool QtCollider::RequestEvent::send( QObjectProxy *proxy, Synchronicity sync )
{
  if( sync == Synchronous ) {
    bool done = false;
    p_done = &done;
    QcApplication::postSyncEvent( this, proxy );
    return done;
  }
  else {
    QApplication::postEvent( proxy, this );
  }

  // WARNING at this point, the event has been deleted, so "this" pointer and data members are
  // not valid anymore!

  return true;
}
