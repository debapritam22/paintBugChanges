#include "shader.h"
#include "util.h"

#define VERSION_STRING "#version 120\n"

QString resourceToString(QString resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "unable to read file path: " << resourcePath.toStdString() << std::endl;
        return QString();
    }

    return file.readAll();
}

QOpenGLShaderProgram* shadersToProgram(QObject *parent, QString vertCode, QString fragCode)
{
    QOpenGLShader* vertShader = new QOpenGLShader(QOpenGLShader::Vertex);
    vertShader->compileSourceCode(vertCode);

    QOpenGLShader* fragShader = new QOpenGLShader(QOpenGLShader::Fragment);
    fragShader->compileSourceCode(fragCode);

    QOpenGLShaderProgram* program = new QOpenGLShaderProgram(parent);
    program->addShader(vertShader);
    program->addShader(fragShader);

    return program;
}

QOpenGLShaderProgram* ShaderFactory::buildMeshShader(QObject *parent)
{
    return shadersToProgram(parent,
                            resourceToString(":/main/resources/shaders/mesh.vert"),
                            resourceToString(":/main/resources/shaders/mesh.frag"));
}

QOpenGLShaderProgram* ShaderFactory::buildBakeShader(QObject *parent)
{
    return shadersToProgram(parent,
                            resourceToString(":/main/resources/shaders/bake.vert"),
                            resourceToString(":/main/resources/shaders/bake.frag"));
}

QOpenGLShaderProgram* ShaderFactory::buildPaintDebugShader(QObject *parent)
{
    return shadersToProgram(parent,
                            resourceToString(":/main/resources/shaders/paint_debug.vert"),
                            resourceToString(":/main/resources/shaders/paint_debug.frag"));
}
