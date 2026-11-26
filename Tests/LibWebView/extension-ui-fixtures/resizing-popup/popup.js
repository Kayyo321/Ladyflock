const toggle = document.getElementById("toggle");
toggle.addEventListener("click", () => {
    document.body.classList.toggle("more");
    toggle.textContent = document.body.classList.contains("more") ? "Less" : "More";
});
