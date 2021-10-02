#ifndef SHADER_H
#define SHADER_H

#include <string>
#include <QOpenGLShaderProgram>

//using namespace std;

class ShaderFactory
{
public:
    static QOpenGLShaderProgram* buildShader(QObject* parent, QString vertFile, QString fragFile);
    static QOpenGLShaderProgram* buildMeshShader(QObject* parent);
    static QOpenGLShaderProgram* buildBakeShader(QObject* parent);
    static QOpenGLShaderProgram* buildPaintDebugShader(QObject* parent);
};

#endif // SHADER_H
