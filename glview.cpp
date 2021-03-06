#include "glview.h"

#include <QOpenGLTexture>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <iostream>

#include "project.h"
#include "sessionsettings.h"
#include "glcache.h"

#define DEBUG_PAINT_LAYER 0

namespace MouseMode {
    enum { FREE, CAMERA, TOOL, HUD };
}

int mouseMode = MouseMode::FREE;
int activeMouseButton = -1;
QList<GLView*> GLView::_glViews;

QOpenGLFramebufferObject* GLView::drawFbo() {
    if (!_drawFbo) {
        _drawFbo = new QOpenGLFramebufferObject(PAINT_FBO_WIDTH, PAINT_FBO_WIDTH, QOpenGLFramebufferObject::Depth);

        // for some reason GL_RGBA32UI doesn't work, maybe because of https://bugreports.qt.io/browse/QTBUG-58312
        _drawFbo->addColorAttachment(PAINT_FBO_WIDTH, PAINT_FBO_WIDTH, GL_RGBA32F);
    }
    return _drawFbo;
}

QOpenGLFramebufferObject* GLView::transferFbo() {
    if (!_transferFbo) {
        _transferFbo = new QOpenGLFramebufferObject(PAINT_FBO_WIDTH, PAINT_FBO_WIDTH);
    }
    return _transferFbo;
}

QOpenGLFramebufferObject* GLView::paintFbo() {
    if (!_paintFbo) {
        QOpenGLFramebufferObjectFormat format;
        format.setInternalTextureFormat(GL_R32F);
        _paintFbo = new QOpenGLFramebufferObject(PAINT_FBO_WIDTH, PAINT_FBO_WIDTH, format);

        _paintFbo->bind();
        glClearColor(0,0,0,0); // red is paint intensity
        glClear(GL_COLOR_BUFFER_BIT);
        _paintFbo->release();
    }
    return _paintFbo;
}

GLView::GLView(QWidget *parent) :
    QOpenGLWidget(parent)
{
    connect(&_messageTimer, SIGNAL(timeout()), this, SLOT(messageTimerUpdate()));
    _messageTimer.setInterval(100);

    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    connect(settings(), SIGNAL(brushSizeChanged()), this, SLOT(brushSizeChanged()));
    connect(settings(), SIGNAL(brushColorChanged(QColor,QColor)), this, SLOT(brushColorChanged(QColor,QColor)));
    connect(Project::activeProject(), SIGNAL(meshAdded()), this, SLOT(onMeshAdded()));
    connect(Project::activeProject(), SIGNAL(meshesRemoved(QList<Mesh*>)), this, SLOT(onMeshesRemoved(QList<Mesh*>)));
    connect(Project::activeProject(), SIGNAL(meshesAltered(QList<Mesh*>)), this, SLOT(onMeshesAltered(QList<Mesh*>)));

    _glViews.append(this); // keep track of all views

    _paintLayerIsDirty = false;
}

QOpenGLTexture* brushTexture = 0;

void GLView::initializeGL()
{
    _meshShader = ShaderFactory::buildMeshShader(this);
    _bakeShader = ShaderFactory::buildBakeShader(this);    
#if DEBUG_PAINT_LAYER
        _paintDebugShader = ShaderFactory::buildPaintDebugShader(this);
#endif
    _logger = new QOpenGLDebugLogger(this);
    _logger->initialize();

    brushTexture = new QOpenGLTexture(QImage(QString(":/main/resources/brushes/brush1.png")));

    if (!QOpenGLContext::currentContext()->functions()->hasOpenGLFeature(QOpenGLFunctions::MultipleRenderTargets)) {
        qDebug("Multiple render targets not supported");
    }
}

void GLView::resizeGL(int w, int h)
{
    glViewport(0, 0, w, h);

    _cameraScratch.viewWidth = w;
    _cameraScratch.viewHeight = h;
}

void GLView::paintGL()
{
      QPainter painter;
    QList<QOpenGLDebugMessage> messages = _logger->loggedMessages();
    foreach (const QOpenGLDebugMessage &message, messages)
        qDebug() << message;


    painter.begin(this);
    painter.beginNativePainting();

    drawScene();

    // draw strokes onto paint FBO
    drawPaintStrokes();

    // draw brush overlay
    //bool cursorInWidget = this->rect().contains(this->mapFromGlobal(QCursor::pos()));
    if (this->underMouse() || mouseMode != MouseMode::FREE) {
        drawBrush();
    }

#if DEBUG_PAINT_LAYER
    drawPaintLayer();
#endif

    painter.endNativePainting();

    drawOutlinedText(&painter, 20, 20, getViewLabel(), QColor(0,0,0), QColor(255,255,255));

    if (_messageTimer.isActive()) {
        QFont prevFont = painter.font();
        QFont bakingFont(prevFont.family(), 20);
        QFontMetrics fm(bakingFont);
        painter.setFont(bakingFont);

        const int bottomOffset = 50;
        QRect bgRect(0, height() - bottomOffset, width(), fm.height());

        painter.fillRect(bgRect, QColor(0,0,0, 200));
        painter.drawText((width()-fm.boundingRect(_busyMessage).width()) / 2, bgRect.y() + bgRect.height() - fm.descent() - 4, _busyMessage);

        painter.setFont(prevFont);
    }

    painter.end();
}

void GLView::drawScene()
{
    glEnable(GL_DEPTH_TEST);

    QOpenGLFramebufferObject* drawTarget = drawFbo();
    if (!drawTarget->bind()) {
        std::cerr << "unable to bind draw target" << std::endl;
    }

    glClearColor(.2,.2,.2,0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    QMatrix4x4 cameraProjM = _camera->getProjMatrix(width(), height());
    QMatrix4x4 cameraViewM = _camera->getViewMatrix(width(), height());
    QMatrix4x4 cameraProjViewM = cameraProjM * cameraViewM;

    Project* project = Project::activeProject();

    // render each mesh
    QVectorIterator<Mesh*> meshes = project->meshes();
    while (meshes.hasNext()) {
        Mesh* mesh = meshes.next();
        if (!project->meshVisible(mesh)) // ignore hidden
            continue;

        QMatrix4x4 objToWorld;

        // make sure a texture exists for this mesh
        if (!GLCache::hasMeshTexture(mesh)) {
            std::cout << "creating mesh texture" << std::endl;

            const int TEXTURE_SIZE = 256;

            transferFbo()->bind();

            GLuint textureId;
            glGenTextures(1, &textureId);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureId);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEXTURE_SIZE, TEXTURE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
            glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            glClearColor(.5,.5,.5,1);
            glClear(GL_COLOR_BUFFER_BIT);

            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0,1,0,1,-1,1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();

            glViewport(0, 0, TEXTURE_SIZE, TEXTURE_SIZE);

            glColor3f(.8,.8,.8);
            glBegin(GL_QUADS);
            {
                glVertex2f(.25,.25);
                glVertex2f(.75,.25);
                glVertex2f(.75,.75);
                glVertex2f(.25,.75);
            }
            glEnd();

            glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, TEXTURE_SIZE, TEXTURE_SIZE, 0);

            transferFbo()->release();

            glViewport(0, 0, width(), height());

            mesh->setTextureSize(TEXTURE_SIZE);
            GLCache::setMeshTexture(mesh, textureId);
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, GLCache::meshTextureId(mesh));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, paintFbo()->texture());
        glActiveTexture(GL_TEXTURE0);

        QColor brushColor = settings()->brushColor();

        _meshShader->bind();
        _meshShader->setUniformValue("objToWorld", objToWorld);
        _meshShader->setUniformValue("cameraPV", cameraProjViewM);
        _meshShader->setUniformValue("paintFboWidth", PAINT_FBO_WIDTH);
        _meshShader->setUniformValue("brushColor", brushColor.redF(), brushColor.greenF(), brushColor.blueF(), 1);
        _meshShader->setUniformValue("meshTexture", 0);
        _meshShader->setUniformValue("paintTexture", 1);

        QOpenGLBuffer *vbo = GLCache::meshVertexBuffer(mesh);
        QOpenGLBuffer *uvbo = GLCache::meshUVBuffer(mesh);
        QOpenGLBuffer *ibo = GLCache::meshIndexBuffer(mesh);
        QOpenGLVertexArrayObject *vao = GLCache::meshVertexArray(mesh);

        QOpenGLBuffer* positionBuffer;
        if (meshVertexSpace() == MeshPropType::UV) {
            positionBuffer = uvbo;
        } else {
            positionBuffer = vbo;
        }

        vao->bind();
        positionBuffer->bind();
        _meshShader->enableAttributeArray("position");
        _meshShader->setAttributeBuffer("position", GL_FLOAT, 0, 3, 0);
        positionBuffer->release();
        uvbo->bind();
        _meshShader->enableAttributeArray("in_uvs");
        _meshShader->setAttributeBuffer("in_uvs", GL_FLOAT, 0, 3, 0);
        uvbo->release();

        QOpenGLExtraFunctions* f = QOpenGLContext::currentContext()->extraFunctions();
        GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        f->glDrawBuffers(2, bufs);

        ibo->bind();
        glDrawElements(GL_TRIANGLES, mesh->_triangleIndices.count(), GL_UNSIGNED_INT, 0);
        ibo->release();
        vao->release();

        _meshShader->release();
    }

    glDisable(GL_DEPTH_TEST);

    if (!drawTarget->release()) {
        std::cerr << "unable to release draw target" << std::endl;
    }

    // draw scene texture to the screen
    drawTarget->texture();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width(), 0, height(), -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, drawTarget->texture());

    glColor3f(1,1,1);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0, 0);
        glVertex2f(0, 0);
        glTexCoord2f(1, 0);
        glVertex2f(PAINT_FBO_WIDTH, 0);
        glTexCoord2f(1, 1);
        glVertex2f(PAINT_FBO_WIDTH, PAINT_FBO_WIDTH);
        glTexCoord2f(0, 1);
        glVertex2f(0, PAINT_FBO_WIDTH);
    }
    glEnd();

    glDisable(GL_TEXTURE_2D);
}

void GLView::drawOutlinedText(QPainter* painter, int x, int y, QString text, QColor bgColor, QColor fgColor)
{
    painter->setPen(bgColor);
    painter->drawText(x, y, text);
    painter->setPen(fgColor);
    painter->drawText(x-2, y-2, text);
}

void GLView::drawBrush()
{
    brushTexture->bind();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width(), 0, height(), -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    QPoint cursorP = this->mapFromGlobal(QCursor::pos());
    cursorP.setY(height() - cursorP.y());
    int brushRadius = settings()->brushSize() * 0.5f;

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glBegin(GL_QUADS);
    {
        glTexCoord2f(0, 0);
        glVertex2f(-brushRadius + cursorP.x(), -brushRadius + cursorP.y());
        glTexCoord2f(1, 0);
        glVertex2f(brushRadius + cursorP.x(), -brushRadius + cursorP.y());
        glTexCoord2f(1, 1);
        glVertex2f(brushRadius + cursorP.x(), brushRadius + cursorP.y());
        glTexCoord2f(0, 1);
        glVertex2f(-brushRadius + cursorP.x(), brushRadius + cursorP.y());
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    brushTexture->release();
    //QPoint cursorP = this->mapFromGlobal(QCursor::pos());
    //painter.drawImage(cursorP, foo);
}

void GLView::messageTimerUpdate()
{
    update();

    if (_messageFinished < QTime::currentTime())
        _messageTimer.stop();
}

void GLView::brushSizeChanged()
{
    update(); // repaint render brush at new size
}

void GLView::brushColorChanged(QColor oldColor, QColor newColor)
{
    if (_paintLayerIsDirty) {
        bakePaintLayer();
    }
}

void GLView::onMeshAdded()
{
    update();
}

void GLView::onMeshesRemoved(QList<Mesh*> removed)
{
    makeCurrent();

    // release textures of removed meshes
    foreach (Mesh* removedMesh, removed) {
        if (GLCache::hasMeshTexture(removedMesh)) {
            GLuint unusedTexture = GLCache::removeMeshTexture(removedMesh);
            glGenTextures(1, &unusedTexture);
        }
    }

    update();
}

void GLView::onMeshesAltered(QList<Mesh *> altered)
{
    update();
}

void GLView::drawPaintStrokes()
{
    if (_strokePoints.size() == 0)
        return;

    float brushRadius = settings()->brushSize() * 0.5f;

    paintFbo()->bind();
    glViewport(0,0,PAINT_FBO_WIDTH,PAINT_FBO_WIDTH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, PAINT_FBO_WIDTH, 0, PAINT_FBO_WIDTH, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    //glBlendEquation(GL_MAX);
    //glAlphaFunc(GL_GREATER, 0.5);
    //glEnable(GL_ALPHA_TEST);

    brushTexture->bind();

    auto drawPoint = [brushRadius](Point2 p) { // helper func
        glTexCoord2f(0, 0);
        glVertex2f(-brushRadius + p.x(), -brushRadius + p.y());
        glTexCoord2f(1, 0);
        glVertex2f(brushRadius + p.x(), -brushRadius + p.y());
        glTexCoord2f(1, 1);
        glVertex2f(brushRadius + p.x(), brushRadius + p.y());
        glTexCoord2f(0, 1);
        glVertex2f(-brushRadius + p.x(), brushRadius + p.y());
    };

    Point2 prevPoint;
    glBegin(GL_QUADS);
    glColor4f(1,1,1,1);
    foreach (Point2 p, _strokePoints) {
        drawPoint(p);
        if (!prevPoint.isNull()) { // draw points in between
            float distance = prevPoint.distanceToPoint(p);
            for (int i = 1; i < distance; i++) {
                float a = i / distance;
                float b = 1 - a;
                Point2 tweenPoint = prevPoint * a + p * b;
                drawPoint(tweenPoint);
            }
        }
        prevPoint = p;
    }
    glEnd();

    brushTexture->release();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    //glDisable(GL_ALPHA_TEST);

    glViewport(0,0,width(),height());
    paintFbo()->release();

    _paintLayerIsDirty = true;
}

void GLView::drawPaintLayer()
{
    QMatrix4x4 cameraProjViewM;
    cameraProjViewM.ortho(0, width(), 0, height(), -1, 1);

    _paintDebugShader->bind();
    _paintDebugShader->setUniformValue("cameraPV", cameraProjViewM);
    _paintDebugShader->setUniformValue("paintTexture", 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D, paintFbo()->texture());

    glBegin(GL_QUADS);
    {
        glTexCoord2f(0,0);
        glVertex2f(0,0);
        glTexCoord2f(1,0);
        glVertex2f(PAINT_FBO_WIDTH,0);
        glTexCoord2f(1,1);
        glVertex2f(PAINT_FBO_WIDTH,PAINT_FBO_WIDTH);
        glTexCoord2f(0,1);
        glVertex2f(0,PAINT_FBO_WIDTH);
    }
    glEnd();

    glDisable(GL_BLEND);
    _paintDebugShader->release();
}

// duration in milliseconds
void GLView::setBusyMessage(QString message, int duration)
{
    _messageFinished = QTime::currentTime().addMSecs(duration);
    _busyMessage = message;

    _messageTimer.start();
}

// called during rendering to bake the paint texture onto the target mesh textures
void GLView::bakePaintLayer()
{
    makeCurrent();
    setBusyMessage("baking", 400);

    Project* project = Project::activeProject();

    transferFbo()->bind();

    QMatrix4x4 cameraProjM = _camera->getProjMatrix(width(), height());
    QMatrix4x4 cameraViewM = _camera->getViewMatrix(width(), height());
    QMatrix4x4 cameraProjViewM = cameraProjM * cameraViewM;

    QMatrix4x4 orthoProjViewM;
    orthoProjViewM.ortho(0,1,0,1,-1,1);

    // render the meshes in UV space onto their texture using the paintFBO
    // render each mesh
    QVectorIterator<Mesh*> meshes = project->meshes();
    while (meshes.hasNext()) {
        Mesh* mesh = meshes.next();
        if (!project->meshVisible(mesh)) // ignore hidden
            continue;

        const int TARGET_TEXTURE_SIZE = mesh->textureSize();
        glViewport(0, 0, TARGET_TEXTURE_SIZE, TARGET_TEXTURE_SIZE);

        QMatrix4x4 objToWorld;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, GLCache::meshTextureId(mesh));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, paintFbo()->texture());
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, drawFbo()->textures()[1]); // want the color attachment with primitive ids
        glActiveTexture(GL_TEXTURE0);

        QVector2D targetScale = QVector2D(width() / (float)PAINT_FBO_WIDTH, height() / (float)PAINT_FBO_WIDTH);

        QColor brushColor = settings()->brushColor();

        _bakeShader->bind();
        _bakeShader->setUniformValue("objToWorld", objToWorld);
        _bakeShader->setUniformValue("orthoPV", orthoProjViewM);
        _bakeShader->setUniformValue("cameraPV", cameraProjViewM);
        _bakeShader->setUniformValue("meshTexture", 0);
        _bakeShader->setUniformValue("paintTexture", 1);
        _bakeShader->setUniformValue("drawTexture", 2);
        _bakeShader->setUniformValue("targetScale", targetScale);
        _bakeShader->setUniformValue("brushColor", brushColor.redF(), brushColor.greenF(), brushColor.blueF(), 1);

        QOpenGLBuffer *vbo = GLCache::meshVertexBuffer(mesh);
        QOpenGLBuffer *uvbo = GLCache::meshUVBuffer(mesh);
        QOpenGLBuffer *ibo = GLCache::meshIndexBuffer(mesh);
        QOpenGLVertexArrayObject *vao = GLCache::meshVertexArray(mesh);

        QOpenGLBuffer* textureBuffer;
        if (meshVertexSpace() == MeshPropType::UV) {
            textureBuffer = uvbo;
        } else {
            textureBuffer = vbo;
        }

        vao->bind();
        uvbo->bind();
        _meshShader->enableAttributeArray("position");
        _meshShader->setAttributeBuffer("position", GL_FLOAT, 0, 3, 0);
        uvbo->release();
        textureBuffer->bind();
        _meshShader->enableAttributeArray("in_uvs");
        _meshShader->setAttributeBuffer("in_uvs", GL_FLOAT, 0, 3, 0);
        textureBuffer->release();

        ibo->bind();
        glDrawElements(GL_TRIANGLES, mesh->_triangleIndices.count(), GL_UNSIGNED_INT, 0);
        ibo->release();
        vao->release();

        _bakeShader->release();

        // copy bake back into mesh texture
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, GLCache::meshTextureId(mesh));
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, TARGET_TEXTURE_SIZE, TARGET_TEXTURE_SIZE, 0);
    }

    transferFbo()->release();

    // clear paint buffer
    paintFbo()->bind();
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);
    paintFbo()->release();

    glViewport(0, 0, width(), height());

    // redraw other views that may be using texture
    foreach (GLView* view, _glViews) {
        if (view != this) {
            view->update();
        }
    }

    _paintLayerIsDirty = false;
}

void GLView::mousePressEvent(QMouseEvent* event)
{
    // handle future keyboard widgets with this
    this->setFocus();

    //bool altDown = event->modifiers() & Qt::AltModifier;
    bool camDown = event->modifiers() & Qt::AltModifier;

    if (mouseMode == MouseMode::FREE && camDown) {
        mouseMode = MouseMode::CAMERA;
        activeMouseButton = event->button();
        _camera->mousePressed(_cameraScratch, event);

        if (_paintLayerIsDirty) {
            bakePaintLayer(); // bake paint layer while aligned with target
        }
    }
    else if (mouseMode == MouseMode::FREE && event->button() & Qt::LeftButton) {
        _strokePoints.append(Point2(event->pos().x(), height()-event->pos().y()));
        mouseMode = MouseMode::TOOL;
        activeMouseButton = event->button();
        _strokePoints.append(Point2(event->pos().x(), height()-event->pos().y()));
    }

    update();
}

void GLView::mouseDoubleClickEvent(QMouseEvent *event)
{
}

void GLView::mouseReleaseEvent(QMouseEvent* event)
{
    //CursorTool* cursorTool = SunshineUi::cursorTool();

    if (mouseMode == MouseMode::CAMERA && event->button() == activeMouseButton) {
        mouseMode = MouseMode::FREE;
        activeMouseButton = -1;
        _camera->mouseReleased(_cameraScratch, event);
    }
    else if (mouseMode == MouseMode::HUD && event->button() == activeMouseButton) {
        mouseMode = MouseMode::FREE;
        activeMouseButton = -1;
    }
    else if (mouseMode == MouseMode::TOOL && event->button() == activeMouseButton) {
        _strokePoints.clear();
        mouseMode = MouseMode::FREE;
        activeMouseButton = -1;
    }

    update();
}

void GLView::mouseMoveEvent(QMouseEvent* event)
{
    if (mouseMode == MouseMode::TOOL) {
        mouseDragEvent(event);

        //_workTool->mouseMoved(event);


    }
    else if (mouseMode != MouseMode::FREE) {
        mouseDragEvent(event);
    }

    update();
}

void GLView::mouseDragEvent(QMouseEvent* event)
{
    if (mouseMode == MouseMode::CAMERA) {
        _camera->mouseDragged(_cameraScratch, event);
    }
    else if (mouseMode == MouseMode::TOOL) {
        _strokePoints.append(Point2(event->pos().x(), height()-event->pos().y()));
    }

    update();
}

void GLView::leaveEvent(QEvent *event)
{
    update();
}

void GLView::resizeEvent(QResizeEvent *event)
{
    QOpenGLWidget::resizeEvent(event);

    if (_paintLayerIsDirty) {
        bakePaintLayer(); // bake paint layer while aligned with target
    }
}

void GLView::keyPressEvent(QKeyEvent *event)
{
    if (mouseMode == MouseMode::FREE) {
        if (event->key() == Qt::Key_Space) {
            bakePaintLayer();
        } else if (event->key() == Qt::Key_BracketLeft) {
            settings()->setBrushSize(settings()->brushSize() - 10);
        } else if (event->key() == Qt::Key_BracketRight) {
            settings()->setBrushSize(settings()->brushSize() + 10);
        }
    }

    // TODO: call base class if not using this event
}
