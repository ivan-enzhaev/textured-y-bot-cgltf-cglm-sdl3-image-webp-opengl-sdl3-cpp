#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>
#include <cstdint>
#include <cstdio>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

// cglm headers for Perspective, View, and Model 3D transformation matrices
#include <cglm/affine.h>
#include <cglm/cam.h>
#include <cglm/cglm.h>
#include <cglm/mat4.h>

// Platform-specific OpenGL headers
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#else
// Desktop (Windows MinGW & Ubuntu Linux)
#include <glad/glad.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/html5.h>
#endif

// Anisotropic filtering extension definitions
#ifndef GL_EXT_texture_filter_anisotropic
#define GL_EXT_texture_filter_anisotropic 1
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

// =============================================================================
// Emscripten Log Output Callback
// =============================================================================
#if defined(__EMSCRIPTEN__)
static void SDLCALL EmscriptenLogCallback(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    (void)category;

    if (priority >= SDL_LOG_PRIORITY_ERROR)
    {
        fprintf(stderr, "%s\n", message);
    }
    else
    {
        fprintf(stdout, "%s\n", message);
    }
}
#endif

// =============================================================================
// Shaders (Combined for Android, Wasm, Windows, and Linux)
// =============================================================================
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define GLSL_VERSION "#version 300 es\n"
#define GLSL_PRECISION "precision mediump float;\n"
#else
#define GLSL_VERSION "#version 330 core\n"
#define GLSL_PRECISION ""
#endif

static const char *vertexShaderSource =
    GLSL_VERSION
    R"(
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

// General Model-View-Projection matrix from cglm
uniform mat4 u_mvp;

void main() {
    gl_Position = u_mvp * vec4(aPos, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char *fragmentShaderSource =
    GLSL_VERSION GLSL_PRECISION
    R"(
out vec4 FragColor;
in vec2 TexCoord;

uniform sampler2D ourTexture;

void main() {
    FragColor = texture(ourTexture, TexCoord);
}
)";

// =============================================================================
// Application State
// =============================================================================
struct AppState
{
    SDL_Window *window = nullptr;
    SDL_GLContext glContext = nullptr;
    GLuint texture = 0;
    GLuint shaderProgram = 0;
    GLuint VAO = 0;
    GLuint posVBO = 0;
    GLuint uvVBO = 0;
    GLuint EBO = 0;
    GLsizei indexCount = 0;
    GLenum indexType = GL_UNSIGNED_SHORT;
    GLint mvpLoc = -1;
    Uint64 startTimeNS = 0; // High-precision nanosecond timestamp
};

// =============================================================================
// Helper: Load Texture from WebP / Image Stream via SDL3_image
// =============================================================================
static GLuint createTexture(const char *path)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (!io)
    {
        SDL_Log("Failed to open file stream for %s: %s", path, SDL_GetError());
        return 0;
    }

    SDL_Surface *surface = IMG_Load_IO(io, 1);
    if (!surface)
    {
        SDL_Log("Image loading failed for %s: %s", path, SDL_GetError());
        return 0;
    }

    SDL_Surface *optimizedSurface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);

    if (!optimizedSurface)
    {
        SDL_Log("Surface conversion failed: %s", SDL_GetError());
        return 0;
    }

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (SDL_GL_ExtensionSupported("GL_EXT_texture_filter_anisotropic"))
    {
        float maxAnisotropy = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, optimizedSurface->w, optimizedSurface->h,
        0, GL_RGBA, GL_UNSIGNED_BYTE, optimizedSurface->pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    SDL_DestroySurface(optimizedSurface);
    return textureID;
}

// =============================================================================
// Helper: Load GLB Model using cgltf
// =============================================================================
static bool loadModelGLB(AppState *state, const char *path)
{
    size_t glbSize = 0;
    void *glbData = SDL_LoadFile(path, &glbSize);
    if (!glbData)
    {
        SDL_Log("Failed to read GLB file %s: %s", path, SDL_GetError());
        return false;
    }

    cgltf_options options = {};
    cgltf_data *data = nullptr;
    if (cgltf_parse(&options, glbData, glbSize, &data) != cgltf_result_success)
    {
        SDL_Log("Failed to parse GLB: %s", path);
        SDL_free(glbData);
        return false;
    }

    cgltf_load_buffers(&options, data, path);

    if (data->meshes_count == 0 || data->meshes[0].primitives_count == 0)
    {
        SDL_Log("GLB has no mesh or primitive: %s", path);
        cgltf_free(data);
        SDL_free(glbData);
        return false;
    }

    cgltf_primitive &prim = data->meshes[0].primitives[0];
    cgltf_accessor *posAccessor = nullptr;
    cgltf_accessor *uvAccessor = nullptr;
    cgltf_accessor *indexAccessor = prim.indices;

    for (cgltf_size i = 0; i < prim.attributes_count; ++i)
    {
        if (prim.attributes[i].type == cgltf_attribute_type_position)
            posAccessor = prim.attributes[i].data;
        else if (prim.attributes[i].type == cgltf_attribute_type_texcoord)
            uvAccessor = prim.attributes[i].data;
    }

    glGenVertexArrays(1, &state->VAO);
    glBindVertexArray(state->VAO);

    // Positions (Location = 0)
    if (posAccessor && posAccessor->buffer_view && posAccessor->buffer_view->buffer)
    {
        const uint8_t *base = static_cast<const uint8_t *>(posAccessor->buffer_view->buffer->data);
        const void *positions = base + posAccessor->buffer_view->offset + posAccessor->offset;
        size_t posSize = posAccessor->count * 3 * sizeof(float);

        glGenBuffers(1, &state->posVBO);
        glBindBuffer(GL_ARRAY_BUFFER, state->posVBO);
        glBufferData(GL_ARRAY_BUFFER, posSize, positions, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    }

    // Texture UVs (Location = 1)
    if (uvAccessor && uvAccessor->buffer_view && uvAccessor->buffer_view->buffer)
    {
        const uint8_t *base = static_cast<const uint8_t *>(uvAccessor->buffer_view->buffer->data);
        const void *uvs = base + uvAccessor->buffer_view->offset + uvAccessor->offset;
        size_t uvSize = uvAccessor->count * 2 * sizeof(float);

        glGenBuffers(1, &state->uvVBO);
        glBindBuffer(GL_ARRAY_BUFFER, state->uvVBO);
        glBufferData(GL_ARRAY_BUFFER, uvSize, uvs, GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    }

    // Element Indices (EBO)
    if (indexAccessor && indexAccessor->buffer_view && indexAccessor->buffer_view->buffer)
    {
        const uint8_t *base = static_cast<const uint8_t *>(indexAccessor->buffer_view->buffer->data);
        const void *indices = base + indexAccessor->buffer_view->offset + indexAccessor->offset;
        size_t indexSize = indexAccessor->count * indexAccessor->stride;

        glGenBuffers(1, &state->EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state->EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexSize, indices, GL_STATIC_DRAW);

        if (indexAccessor->component_type == cgltf_component_type_r_32u)
            state->indexType = GL_UNSIGNED_INT;
        else if (indexAccessor->component_type == cgltf_component_type_r_16u)
            state->indexType = GL_UNSIGNED_SHORT;
        else if (indexAccessor->component_type == cgltf_component_type_r_8u)
            state->indexType = GL_UNSIGNED_BYTE;

        state->indexCount = static_cast<GLsizei>(indexAccessor->count);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    cgltf_free(data);
    SDL_free(glbData);
    return true;
}

// =============================================================================
// Render a Single Frame (Reused in both main loop and window resize)
// =============================================================================
static void renderFrame(AppState *state)
{
    if (!state || !state->window)
        return;

    int display_w = 0;
    int display_h = 0;

#if defined(__EMSCRIPTEN__)
    emscripten_get_canvas_element_size("#canvas", &display_w, &display_h);
#else
    SDL_GetWindowSizeInPixels(state->window, &display_w, &display_h);
#endif

    if (display_w <= 0 || display_h <= 0)
        return;

    // Viewport & Clear
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (state->texture && state->indexCount > 0)
    {
        glUseProgram(state->shaderProgram);

        // ---------------------------------------------------------------------
        // Delta time: 90 degrees/sec rotation around Y-axis
        // ---------------------------------------------------------------------
        Uint64 currentNS = SDL_GetTicksNS();
        double elapsedSeconds = static_cast<double>(currentNS - state->startTimeNS) / 1000000000.0;
        float angleDegrees = static_cast<float>(elapsedSeconds * 90.0);

        // Model Matrix: Rotate Y-Bot around Y-axis
        mat4 model;
        glm_mat4_identity(model);
        vec3 rotAxis = { 0.0f, 1.0f, 0.0f };
        glm_rotate(model, glm_rad(angleDegrees), rotAxis);

        // View Matrix: Frame Y-Bot (same eye, center, up as Qt version)
        mat4 view;
        vec3 eye = { 0.0f, 1.2f, 3.0f };
        vec3 center = { 0.0f, 0.9f, 0.0f };
        vec3 up = { 0.0f, 1.0f, 0.0f };
        glm_lookat(eye, center, up, view);

        // Perspective Projection Matrix
        mat4 projection;
        float aspect = static_cast<float>(display_w) / static_cast<float>(display_h);
        glm_perspective(glm_rad(45.0f), aspect, 0.1f, 100.0f, projection);

        // MVP = Projection * (View * Model)
        mat4 view_model;
        glm_mat4_mul(view, model, view_model);

        mat4 mvp;
        glm_mat4_mul(projection, view_model, mvp);

        if (state->mvpLoc != -1)
        {
            glUniformMatrix4fv(state->mvpLoc, 1, GL_FALSE, reinterpret_cast<const GLfloat *>(mvp));
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, state->texture);

        glBindVertexArray(state->VAO);
        glDrawElements(GL_TRIANGLES, state->indexCount, state->indexType, 0);
        glBindVertexArray(0);
    }

    SDL_GL_SwapWindow(state->window);
}

// =============================================================================
// Direct Browser Resize Callback (Invoked DIRECTLY by the browser)
// =============================================================================
#if defined(__EMSCRIPTEN__)
static EM_BOOL on_browser_resize(int eventType, const EmscriptenUiEvent *uiEvent, void *userData)
{
    (void)eventType;
    AppState *state = static_cast<AppState *>(userData);
    if (!state)
        return EM_TRUE;

    int w = uiEvent->windowInnerWidth;
    int h = uiEvent->windowInnerHeight;
    if (w <= 0 || h <= 0)
        return EM_TRUE;

    emscripten_set_canvas_element_size("#canvas", w, h);
    renderFrame(state);

    return EM_TRUE;
}
#endif

// =============================================================================
// SDL Callbacks
// =============================================================================

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    (void)argc;
    (void)argv;

#if defined(__EMSCRIPTEN__)
    SDL_SetLogOutputFunction(EmscriptenLogCallback, nullptr);
#endif

#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#elif !defined(__EMSCRIPTEN__)
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "60");
#endif

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL initialization failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    AppState *state = new AppState();
    state->startTimeNS = SDL_GetTicksNS();
    *appstate = state;

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

#if defined(__ANDROID__)
    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    state->window = SDL_CreateWindow("Rotating 3D Y-Bot", 0, 0, window_flags);
#else
    SDL_WindowFlags window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    state->window = SDL_CreateWindow("Rotating 3D Y-Bot (SDL3 + OpenGL)", 800, 600, window_flags);
#endif

    if (!state->window)
    {
        SDL_Log("Failed to create SDL window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    state->glContext = SDL_GL_CreateContext(state->window);
    if (!state->glContext)
    {
        SDL_Log("Failed to create OpenGL context: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    SDL_GL_MakeCurrent(state->window, state->glContext);

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    SDL_GL_SetSwapInterval(1);
#else
    SDL_GL_SetSwapInterval(0);
#endif

#if defined(__EMSCRIPTEN__)
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, state, EM_FALSE, on_browser_resize);
#endif

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        SDL_Log("Failed to initialize GLAD!");
        return SDL_APP_FAILURE;
    }
#endif

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // Compile Shaders
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);

    state->shaderProgram = glCreateProgram();
    glAttachShader(state->shaderProgram, vertexShader);
    glAttachShader(state->shaderProgram, fragmentShader);
    glLinkProgram(state->shaderProgram);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    state->mvpLoc = glGetUniformLocation(state->shaderProgram, "u_mvp");
    glUseProgram(state->shaderProgram);
    glUniform1i(glGetUniformLocation(state->shaderProgram, "ourTexture"), 0);
    glUseProgram(0);

#if defined(__ANDROID__)
    const char *modelPath = "models/y-bot/y-bot.glb";
    const char *texturePath = "models/y-bot/y-bot.webp";
#else
    const char *modelPath = "assets/models/y-bot/y-bot.glb";
    const char *texturePath = "assets/models/y-bot/y-bot.webp";
#endif

    if (!loadModelGLB(state, modelPath))
    {
        SDL_Log("Failed to load Y-Bot GLB from %s", modelPath);
    }

    state->texture = createTexture(texturePath);
    if (!state->texture)
    {
        SDL_Log("Failed to load Y-Bot texture from %s", texturePath);
    }

#if defined(__EMSCRIPTEN__)
    double init_w = 0, init_h = 0;
    emscripten_get_element_css_size("#canvas", &init_w, &init_h);
    if (init_w > 0 && init_h > 0)
    {
        emscripten_set_canvas_element_size("#canvas", static_cast<int>(init_w), static_cast<int>(init_h));
    }
#endif

    SDL_Log("Application initialized successfully");
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    (void)appstate;

    if (event->type == SDL_EVENT_QUIT ||
        event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
    {
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    AppState *state = static_cast<AppState *>(appstate);
    renderFrame(state);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)result;

    if (appstate)
    {
        AppState *state = static_cast<AppState *>(appstate);

        if (state->texture)
            glDeleteTextures(1, &state->texture);
        if (state->VAO)
            glDeleteVertexArrays(1, &state->VAO);
        if (state->posVBO)
            glDeleteBuffers(1, &state->posVBO);
        if (state->uvVBO)
            glDeleteBuffers(1, &state->uvVBO);
        if (state->EBO)
            glDeleteBuffers(1, &state->EBO);
        if (state->shaderProgram)
            glDeleteProgram(state->shaderProgram);

        if (state->glContext)
            SDL_GL_DestroyContext(state->glContext);
        if (state->window)
            SDL_DestroyWindow(state->window);

        delete state;
    }

    SDL_Quit();
}
