#ifndef GLVIEW_H
#define GLVIEW_H

#include <QOpenGLWidget>
#include <QOpenGLFramebufferObject>
#include <QColorDialog>
#include <QOpenGLFunctions>
#include <QTimer>

#include <QTime>
#include <QOpenGLShaderProgram>
#include <QOpenGLDebugLogger>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>

#include "transformable.h"
#include "camera.h"
#include "shader.h"
#include "mesh.h"
#include "constants.h"

#define PAINT_FBO_WIDTH 2048

class GLView : public QOpenGLWidget,protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit GLView(QWidget *parent = 0);


    void initializeGL();
    void drawScene();
    void drawOutlinedText(QPainter* painter, int x, int y, QString text, QColor bgColor, QColor fgColor);
    void drawBrush();

    virtual QString getViewLabel() = 0;

     // whether vertices are render with 3D positions or uvs
    virtual MeshPropType meshVertexSpace() = 0;

    void mousePressEvent(QMouseEvent* event);
    void mouseDoubleClickEvent(QMouseEvent *);
    void mouseReleaseEvent(QMouseEvent* event);
    void mouseMoveEvent(QMouseEvent* event);
    void mouseDragEvent(QMouseEvent* event);
    void leaveEvent(QEvent* event);
    void resizeEvent(QResizeEvent *event);
    void keyPressEvent(QKeyEvent* event);
signals:

public slots:
    void messageTimerUpdate();
    void brushSizeChanged();
    void brushColorChanged(QColor oldColor, QColor newColor);
    void onMeshAdded();
    void onMeshesRemoved(QList<Mesh*> removed);
    void onMeshesAltered(QList<Mesh*> altered);
protected:
    void resizeGL(int w, int h);
    void paintGL();
    // shared GL resources
    QOpenGLFramebufferObject* drawFbo();
    QOpenGLFramebufferObject* transferFbo();
    QOpenGLFramebufferObject* paintFbo();

    QOpenGLFramebufferObject* _drawFbo = 0;
    QOpenGLFramebufferObject* _transferFbo = 0;
    QOpenGLFramebufferObject* _paintFbo = 0;

    // non-shared GL resources
    QOpenGLShaderProgram*         _meshShader;
    QOpenGLShaderProgram*         _bakeShader;
    QOpenGLShaderProgram*         _paintDebugShader;

    Camera* _camera;
    CameraScratch             _cameraScratch;

    static QList<GLView*> _glViews;

    void drawPaintStrokes();
    void drawPaintLayer();

    void setBusyMessage(QString message, int duration);

private:
    void                     bakePaintLayer();

    QList<Point2>             _strokePoints;
    bool                      _paintLayerIsDirty;

    QTimer _messageTimer;
    QString _busyMessage = "";
    QTime _messageFinished;
    QOpenGLDebugLogger* _logger;

};

#endif // GLVIEW_H
