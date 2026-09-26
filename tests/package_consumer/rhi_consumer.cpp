#include <vnm_msdf_text/rhi/text_renderer.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtGui/private/qshader_p.h>

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    vnm::msdf_text::rhi::Text_renderer renderer;
    for (const char* shader : {"msdf_text.vert.qsb", "msdf_text.frag.qsb",
                              "msdf_text_styled.vert.qsb", "msdf_text_styled.frag.qsb"})
    {
        QFile file(QStringLiteral(":/vnm_msdf_text/shaders/rhi/") + QString::fromLatin1(shader));
        if (!file.open(QIODevice::ReadOnly) || !QShader::fromSerialized(file.readAll()).isValid()) {
            std::fprintf(stderr, "installed RHI shader resource unavailable: %s\n", shader);
            return 1;
        }
    }
    renderer.release_resources();
    return 0;
}
